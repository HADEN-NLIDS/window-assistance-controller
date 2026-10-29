/*
 * cli.c - see cli.h for the subsystem problem and the mutex ordering.
 *
 * The commands are implemented here rather than in the window code because
 * every one of them has to work with the window closed, and because the same
 * operations are what a user will eventually reach through the UI - so the
 * logic has to exist somewhere both can call.
 *
 * Nothing in this file opens a window, creates a tray icon, or touches the
 * configuration beyond [rule:*]. A command-line run must leave no trace other
 * than what it was asked to do.
 */

#include "cli.h"

#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../core/app_id.h"
#include "../core/launch.h"
#include "../core/monitor.h"
#include "../core/relcoord.h"
#include "../core/rule_apply.h"
#include "../core/win_enum.h"
#include "../core/win_place.h"
#include "../model/config.h"
#include "../model/pickup.h"   /* wac pick: read the layout off the screen */
#include "../model/rule.h"
#include "../model/runstate.h"
#include "../model/store.h"
#include "../model/workspace.h"
#include "../model/workspace_run.h"
#include "../util/strutil.h"

/* ------------------------------------------------------------------ */
/* Static storage                                                      */
/*                                                                     */
/* WindowInfo is ~1.3 KB, RuleSet ~70 KB and WorkspaceSet ~1.4 MB, so   */
/* none of them belongs on the stack - see the sizing warnings in       */
/* win_enum.h, rule.h and workspace.h.                                  */
/* ------------------------------------------------------------------ */

static WindowInfo         g_wins[WAC_WIN_MAX];
static MonitorFingerprint g_mons[WAC_MON_MAX];
static int                g_mon_count;
static RuleSet            g_rules;
static WorkspaceSet       g_workspaces;

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void out(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
}

static int eq(const wchar_t *a, const wchar_t *b)
{
    return (a != NULL && b != NULL && _wcsicmp(a, b) == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

BOOL cli_attach_console(void)
{
    FILE *f;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    int have_out = (hOut != NULL && hOut != INVALID_HANDLE_VALUE);
    int have_in  = (hIn  != NULL && hIn  != INVALID_HANDLE_VALUE);

    /*
     * AN INHERITED HANDLE BEATS A CONSOLE, AND MUST BE KEPT.
     *
     * MEASURED: the first version reopened CONOUT$ unconditionally, so
     *
     *     wac rules > rules.txt
     *
     * wrote every byte to the screen and left an empty file. Redirection is
     * the whole reason a command line exists; stealing its output to a
     * console is worse than having no console at all.
     *
     * A GUI-subsystem process usually inherits NOTHING, which is why the
     * attach below is still needed for the ordinary case.
     */
    if (!have_out) {
        if (GetConsoleWindow() == NULL) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                /*
                 * No parent console - started from a shortcut, a GUI host, or
                 * a double-click. Allocate one so the output is not lost: a
                 * command that appears to do nothing is worse than one that
                 * opens a window saying what it did.
                 */
                if (!AllocConsole()) return FALSE;
            }
        }

        /*
         * The C runtime still holds whatever it inherited, which for a
         * GUI-subsystem process is nothing. Attaching the console does not
         * update those stream pointers; only reopening them does. Without
         * these lines every wprintf goes nowhere at all.
         */
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    if (!have_in && GetConsoleWindow() != NULL) {
        freopen_s(&f, "CONIN$", "r", stdin);
    }

    /* The code page and the C locale matter for a console; for a redirected
     * file the bytes are already UTF-8 by way of setlocale, which is set
     * below either way. */
    if (GetConsoleWindow() != NULL) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    /*
     * wprintf converts wide characters to bytes through the C locale, and the
     * default "C" locale can represent nothing outside ASCII - a Chinese
     * window title comes out as '?' the moment stdout is a pipe or a file
     * rather than a console. ".UTF-8" is available from Windows 10 1803; a
     * failure simply keeps the old behaviour.
     */
    setlocale(LC_ALL, ".UTF-8");

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Verb recognition                                                    */
/* ------------------------------------------------------------------ */

/*
 * The verbs that mean "this is a command-line run, do not open the window".
 *
 * THIS LIST MUST MATCH cli_run() BELOW. It did not, and the consequence was
 * invisible for a long time: `pick` was implemented and dispatched by cli_run()
 * but missing here, so `wac pick` never reached cli_run() at all. main.c saw
 * "not a command", opened the main window instead, and the program exited 0
 * having done nothing - no output (a GUI-subsystem program has no console
 * anyway), no config change, and one ordinary-looking start-up/shutdown pair in
 * the log.
 *
 * That is what "wac pick does not write the file, cause unknown" (docs/进度记录.md,
 * 2026-09-22) actually was: the command never ran. MEASURED 2026-10-11 -
 * calling pickup_layout() directly from a console test (tests/t73_pickup.c)
 * turns four windows into four presets in 0 ms, so the function was never the
 * problem.
 *
 * Two lists for one fact is the same mistake as two functions computing one
 * size. If a verb is added to cli_run(), it goes here in the same edit.
 */
static const wchar_t *const k_verbs[] = {
    L"help", L"list", L"monitors", L"rules", L"apply", L"place", L"version",
    L"run", L"close", L"pick"
};

BOOL cli_is_command(int argc, wchar_t **argv)
{
    if (argv == NULL || argc < 2) return FALSE;
    if (argv[1] == NULL || argv[1][0] == L'\0') return FALSE;

    /* A leading dash is a question about the program, not a request to open
     * its window. Anything else unknown is treated as "no verb" so that a
     * stray argument cannot silently suppress the window. */
    if (argv[1][0] == L'-') {
        return (eq(argv[1], L"--help") || eq(argv[1], L"-h") ||
                eq(argv[1], L"--version") || eq(argv[1], L"-v")) ? TRUE : FALSE;
    }

    for (int i = 0; i < (int)ARRAYSIZE(k_verbs); ++i) {
        if (eq(argv[1], k_verbs[i])) return TRUE;
    }
    return FALSE;
}

void cli_usage(void)
{
    out(L"wac - window assistance controller\n");
    out(L"\n");
    out(L"  wac                              open the main window\n");
    out(L"  wac help                         this text\n");
    out(L"\n");
    out(L"  wac list [--all]                 windows this tool can place\n");
    out(L"  wac monitors                     monitors and their fingerprints\n");
    out(L"\n");
    out(L"  wac rules                        list the stored placement rules\n");
    out(L"  wac rules --forget <name>        delete a rule\n");
    out(L"  wac apply                        put every matching window back\n");
    out(L"\n");
    out(L"  wac run <workspace>              start a workspace and place it\n");
    out(L"  wac run                          list the stored workspaces\n");
    out(L"  wac close <workspace> [--yes]     end what that workspace started\n");
    out(L"       (lists the processes first; --yes is required to act)\n");
    out(L"\n");
    out(L"  wac place --app <name|path> [options]\n");
    out(L"  wac place --pid <n>       [options]\n");
    out(L"  wac place --hwnd <0x...>  [options]\n");
    out(L"  wac place --launch \"<command>\" [options]\n");
    out(L"    --zone x,y,w,h    normalized area (default: the stored rule)\n");
    out(L"    --monitor <n>     target monitor (default: the window's own)\n");
    out(L"    --move-only       never change the size\n");
    out(L"    --remember        save the placement as a rule\n");
    out(L"    --wait <ms>       window wait for --launch (default 15000)\n");
    out(L"    --verbose         show the wait loop's progress\n");
    out(L"\n");
    out(L"  Exit codes: 0 done, 1 could not do it, 2 bad command line.\n");
    out(L"\n");
    out(L"  Note: this program is a WINDOWS-subsystem executable, so cmd.exe does\n");
    out(L"  not wait for it and the prompt reappears before the output. Use\n");
    out(L"      start /wait wac ...\n");
    out(L"  in a batch file when the order matters.\n");
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

static void load_environment(void)
{
    g_mon_count = monitor_enumerate(g_mons, WAC_MON_MAX);
}

/*
 * Commands that need the whole configuration read it in two pieces: the rules
 * come from rules_load() (which owns the [rule:*] half) and the workspaces
 * from load_workspaces(). Both go through store_load, so a damaged file is
 * discovered twice and would be announced twice - which reads as two separate
 * problems. Once is the honest number.
 */
static BOOL g_config_reported = FALSE;

static void report_config_problem(StoreLoadResult r)
{
    if (g_config_reported) return;

    if (r == STORE_CORRUPT_NO_RECOVERY) {
        out(L"warning: the configuration is damaged and no backup was usable.\n");
        g_config_reported = TRUE;
    } else if (r == STORE_RECOVERED_FROM_BACKUP) {
        out(L"note: the configuration was damaged; a backup was loaded.\n");
        g_config_reported = TRUE;
    } else if (r == STORE_MODIFIED_EXTERNALLY) {
        /* Not a problem - the contents are in use. Said out loud because the
         * alternative used to be loading an older file in silence. */
        out(L"note: the configuration was changed outside this program; using it as it is.\n");
        g_config_reported = TRUE;
    }
}

static RulesLoadResult load_rules_reporting(void)
{
    RulesLoadResult lr = rules_load(&g_rules);

    if (lr == RULES_RECOVERED) {
        report_config_problem(STORE_RECOVERED_FROM_BACKUP);
    } else if (lr == RULES_MODIFIED) {
        report_config_problem(STORE_MODIFIED_EXTERNALLY);
    } else if (lr == RULES_UNAVAILABLE) {
        out(L"warning: the configuration could not be read, so no rules are available.\n");
    }
    return lr;
}

/*
 * Load the workspaces from the same file the rules come from.
 *
 * A missing file is not reported: "nothing stored yet" is the normal first
 * run, and an empty list already says so. A file that EXISTS but cannot be
 * read is reported, because that one looks identical in the data and means
 * something completely different - the same distinction rules_load draws.
 */
static void load_workspaces(void)
{
    wchar_t path[MAX_PATH];
    if (!config_file(path, MAX_PATH)) return;

    StoreDocument *doc = NULL;
    const StoreLoadResult r = store_load(path, &doc);

    report_config_problem(r);
    if (doc == NULL) return;

    ws_from_store(&g_workspaces, doc);
    store_destroy(doc);
}

/* ------------------------------------------------------------------ */
/* run                                                                 */
/* ------------------------------------------------------------------ */

/* workspace_run reports through a callback so the same code serves the window
 * and the command line; here it becomes one line of output each. */
static void run_trace(void *ctx, const wchar_t *line)
{
    (void)ctx;
    out(L"%ls\n", line);
}

static int cmd_run(int argc, wchar_t **argv)
{
    load_workspaces();
    load_rules_reporting();

    /* No argument: list what there is. More useful than a usage line when the
     * user is trying to remember what they called it. */
    if (argc < 1 || argv[0] == NULL || argv[0][0] == L'\0') {
        if (g_workspaces.count == 0) {
            out(L"no workspaces stored yet.\n");
            return 0;
        }
        out(L"workspaces:\n");
        for (int i = 0; i < g_workspaces.count; ++i) {
            out(L"  %-20hs  %d slot(s)\n", g_workspaces.items[i].id,
                g_workspaces.items[i].slot_count);
        }
        return 0;
    }

    char id[WS_ID_MAX];
    if (!wide_to_str(argv[0], id, sizeof(id)) || id[0] == '\0') {
        out(L"that is not a usable workspace name.\n");
        return 2;
    }

    const Workspace *w = ws_find_const(&g_workspaces, id);
    if (w == NULL) {
        out(L"no workspace called \"%hs\".\n", id);
        if (g_workspaces.count > 0) {
            out(L"stored: ");
            for (int i = 0; i < g_workspaces.count; ++i) {
                out(L"%hs%s", g_workspaces.items[i].id,
                    (i + 1 < g_workspaces.count) ? L", " : L"\n");
            }
        }
        return 1;
    }

    if (w->slot_count == 0) {
        out(L"workspace \"%hs\" has no applications in it yet.\n", w->id);
        out(L"add [slot:%hs:0] sections to the configuration file.\n", w->id);
        return 1;
    }

    /*
     * No LaunchGroup: this process is about to exit, and a job object dies
     * with it - which would kill the programs it just started. The handles
     * belong to nobody here, so the applications stay up, which is what
     * "start my workspace" means.
     */
    const WorkspaceRunReport rep = workspace_run(w, &g_rules, NULL,
                                                 run_trace, NULL);

    /*
     * Write down what was started, so `wac close` can end it later. Without
     * this the pids are gone the moment this process exits, and the only way
     * left to close a workspace would be to guess from process names - which
     * would kill the user's own copies too.
     */
    {
        RunState st;
        runstate_init(&st, w->id);

        for (int i = 0; i < rep.count; ++i) {
            if (rep.slots[i].pid != 0) runstate_add(&st, rep.slots[i].pid, NULL);

            /*
             * AND THE PROCESS THAT OWNS THE WINDOW, which is often a different
             * one. Windows 11 Notepad, Calculator and every Store application
             * start a launcher that hands the window to another process - the
             * very thing core/launch.c exists for. Recording only the launcher
             * means `wac close` ends something invisible and leaves the window
             * the user is looking at on screen. Measured: closing the recorded
             * pid left one of two Notepad processes running.
             */
            if (rep.slots[i].hwnd != NULL) {
                DWORD owner = 0;
                GetWindowThreadProcessId(rep.slots[i].hwnd, &owner);
                if (owner != 0 && owner != rep.slots[i].pid) {
                    runstate_add(&st, owner, NULL);
                }
            }
        }

        if (st.count > 0) {
            if (runstate_save(&st)) {
                out(L"note: recorded %d process(es); \"wac close %hs\" will end them.\n",
                    st.count, w->id);
            } else {
                /* Said out loud: the consequence is that this workspace cannot
                 * be closed automatically later. */
                out(L"warning: could not record what was started, so `wac close` "
                    L"will not know about it.\n");
            }
        }
    }

    return (rep.ok == rep.count) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* close                                                               */
/* ------------------------------------------------------------------ */

static void close_trace(void *ctx, const wchar_t *line)
{
    (void)ctx;
    out(L"%ls\n", line);
}

static int cmd_close(int argc, wchar_t **argv)
{
    load_workspaces();

    if (argc < 1 || argv[0] == NULL || argv[0][0] == L'\0') {
        out(L"usage: wac close <workspace> [--yes]\n");
        out(L"       lists the processes a workspace started, and ends them\n");
        return 2;
    }

    BOOL confirmed = FALSE;
    for (int i = 1; i < argc; ++i) {
        if (eq(argv[i], L"--yes") || eq(argv[i], L"-y")) confirmed = TRUE;
    }

    char id[WS_ID_MAX];
    if (!wide_to_str(argv[0], id, sizeof(id)) || id[0] == '\0') {
        out(L"that is not a usable workspace name.\n");
        return 2;
    }

    /* Checked against the configuration first: "nothing recorded" and "no
     * such workspace" are different answers, and only one of them means the
     * user should look at what they typed. */
    if (ws_find_const(&g_workspaces, id) == NULL) {
        out(L"no workspace called \"%hs\".\n", id);
        return 1;
    }

    RunState st;
    if (!runstate_load(id, &st) || st.count == 0) {
        out(L"nothing recorded for \"%hs\".\n", id);
        out(L"Either it was not started by this program, or it has already been closed.\n");
        return 1;
    }

    /*
     * Closing a workspace is the only destructive thing this program does, so
     * the list comes first and the action needs --yes. The window uses a
     * confirmation dialog for the same reason; a command line cannot, and
     * silently ending six programs because someone typed a name is worse than
     * asking them to type one more word.
     */
    out(L"workspace \"%hs\" would end %d process(es):\n", id, st.count);
    for (int i = 0; i < st.count; ++i) {
        out(L"  pid %-8lu %ls\n", (unsigned long)st.entries[i].pid, st.entries[i].exe);
    }

    if (!confirmed) {
        out(L"\nnothing was ended. Add --yes to confirm.\n");
        return 1;
    }

    out(L"\n");
    int failed = 0, stale = 0;
    const int ended = runstate_terminate(&st, close_trace, NULL, &failed, &stale);

    out(L"\nended %d, already gone %d, failed %d\n", ended, stale, failed);

    /*
     * Said rather than implied: this path ends the processes that were
     * recorded, one by one. A job object would also reach their children (a
     * browser's helper processes); this cannot, and a user who expects the
     * whole tree gone should know which of the two they are getting.
     */
    out(L"note: this ends the recorded processes only. Anything they started\n"
        L"      themselves is left running.\n");

    /*
     * The record is dropped either way. Leaving it would make a second `wac
     * close` report processes that are gone, and worse, a recycled pid could
     * match a stale entry. Forgetting is the safe direction.
     */
    runstate_forget(id);

    return (failed == 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* list                                                                */
/* ------------------------------------------------------------------ */

static void print_window_line(const WindowInfo *w, WinFilterReason why)
{
    out(L"  %-30ls %8lu  %5dx%-5d at %6d,%-6d  %ls\n",
        why == WIN_OK ? L"OK" : win_filter_reason_name(why),
        (unsigned long)w->pid,
        (int)(w->rect.right - w->rect.left), (int)(w->rect.bottom - w->rect.top),
        (int)w->rect.left, (int)w->rect.top,
        w->title[0] ? w->title : L"(no title)");

    if (w->exe_path[0] != L'\0') {
        out(L"      %ls\n", w->exe_path);
    }
}

/*
 * wac pick - read the current layout off the screen and store it as presets.
 *
 * The command-line twin of the overlay's [pick up this layout] button, and the
 * shortest path from "my desktop is arranged the way I like it" to "and it
 * stays that way". It earns a verb of its own rather than a flag on `apply`
 * because it does the opposite of apply: apply imposes the stored layout on the
 * screen, pick reads the screen into the store.
 */
static int cmd_pick(void)
{
    PickupSummary sum;
    ZeroMemory(&sum, sizeof(sum));

    /*
     * Read the stored rules FIRST.
     *
     * MEASURED 2026-09-22: without this, `wac pick` appeared to hang. The cause
     * was not the window scan. load_environment(), which the dispatcher runs
     * before this function, only enumerates monitors - it never touches
     * g_rules. Every other command that uses g_rules calls
     * load_rules_reporting() itself, and this one did not, so rules_upsert()
     * was handed a RuleSet that had never been initialized.
     *
     * The shape of the bug is worth keeping: "the dispatcher loads the
     * environment" was true of monitors and false of rules, and the name did
     * not say which.
     *
     * The "hang" itself was a misreading - wac.exe is a WINDOWS-subsystem
     * program, so the shell does not wait for it, and what looked like a stuck
     * process was the main window opening afterwards. The missing call above is
     * a real bug and the fix is needed either way.
     *
     * STILL UNRESOLVED (2026-09-23): this command does not write the file.
     * pickup_layout() either returns FALSE or finds nothing to pick, and the
     * CLI is a poor place to find out which - a GUI-subsystem program cannot be
     * trusted to report. The overlay's [pick up this layout] button is the
     * better place to finish this.
     */
    (void)load_rules_reporting();

    if (!pickup_layout(&g_rules, &sum)) {
        out(L"nothing to pick up: no windows, or no monitors reported.\n");
        return 1;
    }

    if (sum.picked <= 0) {
        out(L"no window could be turned into a preset (%d examined).\n",
            sum.scanned);
        return 1;
    }

    if (!rules_save(&g_rules)) {
        out(L"the presets could not be written to the config file.\n");
        return 1;
    }

    out(L"picked up %d window(s); %d preset(s) stored.\n",
        sum.picked, rules_count(&g_rules));
    if (sum.maximized > 0) {
        out(L"  %d maximized window(s) recorded as the whole monitor\n",
            sum.maximized);
    }
    if (sum.skipped > 0) {
        out(L"  %d window(s) skipped: no usable identity\n", sum.skipped);
    }
    out(L"\nrun 'wac rules' to see them, or 'wac apply' to put them back.\n");
    return 0;
}

static int cmd_list(int argc, wchar_t **argv)
{
    int all = 0;
    int n, accepted = 0, rejected = 0;

    for (int i = 0; i < argc; ++i) {
        if (eq(argv[i], L"--all")) all = 1;
    }

    n = win_enum_top(g_wins, WAC_WIN_MAX, WIN_ENUM_NEED_EXE | WIN_ENUM_NEED_TITLE);

    for (int i = 0; i < n; ++i) {
        DWORD pid = 0;
        WinFilterReason why = win_classify(g_wins[i].hwnd, &pid);
        if (why == WIN_OK) accepted++; else rejected++;
    }

    out(L"%d top-level window(s): %d can be placed, %d rejected\n\n", n, accepted, rejected);

    if (n >= WAC_WIN_MAX) {
        out(L"warning: the scan hit its cap of %d, so some windows were not examined.\n\n",
            WAC_WIN_MAX);
    }

    for (int i = 0; i < n; ++i) {
        DWORD pid = 0;
        WinFilterReason why = win_classify(g_wins[i].hwnd, &pid);
        if (!all && why != WIN_OK) continue;
        print_window_line(&g_wins[i], why);
    }

    if (!all && rejected > 0) {
        out(L"\n%d rejected window(s) hidden - use --all to see them and why.\n",
            rejected);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* monitors                                                            */
/* ------------------------------------------------------------------ */

static int cmd_monitors(void)
{
    out(L"%d monitor(s)\n", g_mon_count);

    for (int i = 0; i < g_mon_count; ++i) {
        RECT rc;
        ZeroMemory(&rc, sizeof(rc));

        out(L"\n[%d]\n", i);
        out(L"    device        : %ls\n", g_mons[i].device_name);
        out(L"    friendly name : %ls\n",
            g_mons[i].friendly_name[0] ? g_mons[i].friendly_name : L"(none read)");
        out(L"    resolution    : %dx%d\n", g_mons[i].width, g_mons[i].height);
        out(L"    origin        : %d,%d\n", g_mons[i].virtual_x, g_mons[i].virtual_y);
        out(L"    primary       : %ls\n", g_mons[i].is_primary ? L"yes" : L"no");
        out(L"    dpi           : %u\n", g_mons[i].dpi);

        if (monitor_rect(g_mons, g_mon_count, i, &rc)) {
            out(L"    rcMonitor     : (%d,%d)-(%d,%d)\n",
                (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* rules                                                               */
/* ------------------------------------------------------------------ */

static void print_rule(int index, const Rule *r)
{
    wchar_t app[WAC_EXE_MAX + WAC_CLASS_MAX + 32];

    app[0] = L'\0';
    app_id_describe(&r->app, app, ARRAYSIZE(app));

    out(L"  [%d] %ls\n", index, app);
    out(L"      zone : x=%.4f y=%.4f w=%.4f h=%.4f\n",
        (double)r->zone.x, (double)r->zone.y,
        (double)r->zone.w, (double)r->zone.h);

    if (r->zone.abs_w > 0 && r->zone.abs_h > 0) {
        out(L"      size : remembered %dx%d px (measured on a %d px wide monitor)\n",
            r->zone.abs_w, r->zone.abs_h, r->zone.ref_w);
    } else {
        out(L"      size : a share of the screen - follows the panel's shape\n");
    }

    if (!r->enabled) out(L"      disabled\n");

    if (r->id[0] != '\0') {
        wchar_t wid[RULE_ID_MAX];
        wid[0] = L'\0';
        if (MultiByteToWideChar(CP_UTF8, 0, r->id, -1, wid, RULE_ID_MAX) > 0) {
            out(L"      id   : %ls\n", wid);
        }
    }
}

static int cmd_rules(int argc, wchar_t **argv)
{
    const wchar_t *forget = NULL;

    for (int i = 0; i < argc; ++i) {
        if (eq(argv[i], L"--forget") && i + 1 < argc) forget = argv[i + 1];
    }

    load_rules_reporting();

    if (forget != NULL) {
        int idx = rules_find_by_name(&g_rules, forget);

        if (idx < 0) {
            out(L"no rule matches \"%ls\" - run 'wac rules' to see the list.\n", forget);
            return 1;
        }

        print_rule(idx, rules_at(&g_rules, idx));
        if (!rules_remove(&g_rules, idx)) {
            out(L"could not remove it.\n");
            return 1;
        }
        if (!rules_save(&g_rules)) {
            out(L"could not write the configuration; the rule is still there on disk.\n");
            return 1;
        }
        out(L"removed; %d rule(s) left.\n", rules_count(&g_rules));
        return 0;
    }

    if (rules_count(&g_rules) == 0) {
        wchar_t path[MAX_PATH];
        out(L"no rules yet.\n");
        if (config_file(path, MAX_PATH)) out(L"file: %ls\n", path);
        out(L"\nrecord one:\n");
        out(L"  wac place --app notepad.exe --zone 0,0,0.5,1 --remember\n");
        out(L"  wac place --app notepad.exe --remember     (keeps where it is now)\n");
        return 0;
    }

    out(L"%d rule(s):\n\n", rules_count(&g_rules));
    for (int i = 0; i < rules_count(&g_rules); ++i) {
        print_rule(i, rules_at(&g_rules, i));
        out(L"\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Target rectangle                                                    */
/* ------------------------------------------------------------------ */

/*
 * Position always comes from the fractions: "where on the screen" is exactly
 * what a normalized coordinate means. The SIZE comes from the rule when it
 * remembers one, and from the fractions otherwise - see rule.h for why that
 * distinction exists and what it cost to learn.
 */
static BOOL zone_rect(float zx, float zy, float zw, float zh,
                      const RECT *mon, RECT *out)
{
    RelRect z;
    POINT pt;
    SIZE sz;

    ZeroMemory(&z, sizeof(z));
    z.x = zx; z.y = zy; z.w = zw; z.h = zh;
    z.min_w = 1; z.min_h = 1;

    if (!rel_to_pixels(&z, mon, &pt, &sz)) return FALSE;

    out->left   = pt.x;
    out->top    = pt.y;
    out->right  = pt.x + sz.cx;
    out->bottom = pt.y + sz.cy;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* apply                                                               */
/* ------------------------------------------------------------------ */

/* Per-window report for `wac apply`. The loop itself lives in
 * core/rule_apply.c so the window's [Apply all] button can share it. */
static void apply_report(const WindowInfo *w, const Rule *r,
                         const RECT *target, PlaceResult res,
                         const PlaceReport *rep, void *ctx)
{
    const int verbose = (ctx != NULL) ? *(const int *)ctx : 0;
    (void)r;

    out(L"  %ls\n", w->title);

    if (target != NULL) {
        out(L"    pid %lu -> (%d,%d)-(%d,%d) %dx%d\n",
            (unsigned long)w->pid,
            (int)target->left, (int)target->top,
            (int)target->right, (int)target->bottom,
            (int)(target->right - target->left),
            (int)(target->bottom - target->top));
    }

    out(L"    %ls  (pos error %d px, size error %d px)\n",
        win_place_result_name(res),
        (rep != NULL) ? rep->pos_error : 0,
        (rep != NULL) ? rep->size_error : 0);

    if (res == PLACE_DENIED) {
        out(L"    the target runs elevated; run this program elevated too.\n");
    }
    if (verbose && rep != NULL && rep->fg_before != rep->fg_after) {
        out(L"    warning: the foreground window changed during placement.\n");
    }
}

static int cmd_apply(int argc, wchar_t **argv)
{
    int move_only = 0;
    int verbose = 0;

    for (int i = 0; i < argc; ++i) {
        if (eq(argv[i], L"--move-only")) move_only = 1;
        else if (eq(argv[i], L"--verbose")) verbose = 1;
    }

    load_rules_reporting();

    const int rule_count = rules_count(&g_rules);
    if (rule_count == 0) {
        out(L"no rules to apply - see 'wac rules'.\n");
        return 1;
    }

    out(L"applying %d rule(s)\n\n", rule_count);

    RuleApplySummary sum;
    if (!rule_apply_all(&g_rules, move_only ? TRUE : FALSE,
                        apply_report, &verbose, &sum)) {
        out(L"nothing to do: no placeable windows are open.\n");
        return 1;
    }

    out(L"\n%d window(s) scanned, %d matched, %d placed",
        sum.scanned, sum.matched, sum.placed);
    if (sum.denied  > 0) out(L", %d denied (elevated target)", sum.denied);
    if (sum.failed  > 0) out(L", %d failed", sum.failed);
    out(L".\n");

    return (sum.matched == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* place                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    AppId   sel;
    wchar_t launch_cmd[WAC_LAUNCH_CMD_MAX];

    int     have_zone;
    float   zx, zy, zw, zh;

    int     monitor_index;   /* -1 = follow the window */
    int     move_only;
    int     remember;
    int     verbose;
    int     wait_ms;
} PlaceOptions;

static int parse_place(int argc, wchar_t **argv, PlaceOptions *o)
{
    ZeroMemory(o, sizeof(*o));
    o->monitor_index = -1;
    o->wait_ms = 15000;

    for (int i = 0; i < argc; ++i) {
        const wchar_t *a = argv[i];

        if (eq(a, L"--app") && i + 1 < argc)         app_id_set_exe(&o->sel, argv[++i]);
        else if (eq(a, L"--pid") && i + 1 < argc)    o->sel.pid = (DWORD)wcstoul(argv[++i], NULL, 10);
        else if (eq(a, L"--hwnd") && i + 1 < argc)   o->sel.hwnd = (HWND)(ULONG_PTR)_wcstoui64(argv[++i], NULL, 0);
        else if (eq(a, L"--launch") && i + 1 < argc) wcsncpy_s(o->launch_cmd, WAC_LAUNCH_CMD_MAX, argv[++i], _TRUNCATE);
        else if (eq(a, L"--class") && i + 1 < argc)  wcsncpy_s(o->sel.class_name, WAC_CLASS_MAX, argv[++i], _TRUNCATE);
        else if (eq(a, L"--title") && i + 1 < argc)  wcsncpy_s(o->sel.title_sub, WAC_TITLE_MAX, argv[++i], _TRUNCATE);
        else if (eq(a, L"--move-only"))              o->move_only = 1;
        else if (eq(a, L"--remember"))               o->remember = 1;
        else if (eq(a, L"--verbose"))                o->verbose = 1;
        else if (eq(a, L"--monitor") && i + 1 < argc) o->monitor_index = (int)wcstol(argv[++i], NULL, 10);
        else if (eq(a, L"--wait") && i + 1 < argc) {
            o->wait_ms = (int)wcstol(argv[++i], NULL, 10);
            if (o->wait_ms < 100) o->wait_ms = 100;
        }
        else if (eq(a, L"--zone") && i + 1 < argc) {
            if (swscanf_s(argv[++i], L"%f,%f,%f,%f",
                          &o->zx, &o->zy, &o->zw, &o->zh) != 4) {
                out(L"--zone wants x,y,w,h in normalized units, e.g. 0,0,0.5,1\n");
                return -1;
            }
            o->have_zone = 1;
        }
        else {
            out(L"unknown option: %ls\n", a);
            return -1;
        }
    }

    if (!o->have_zone && o->launch_cmd[0] == L'\0' &&
        app_id_empty(&o->sel)) {
        out(L"nothing to place - pass --app, --pid, --hwnd or --launch.\n");
        return -1;
    }
    return 0;
}

/* Set by the launch callback so the CLI can print progress lines. */
static void trace_to_console(void *ctx, const wchar_t *line)
{
    (void)ctx;
    out(L"  %ls\n", line);
}

static int cmd_place(int argc, wchar_t **argv)
{
    PlaceOptions o;
    Launch launched;
    HWND target = NULL;
    WindowInfo info;
    RECT cur, mon, target_rect;
    RelRect remember;
    int have_remember = 0;
    int mon_index, rule_index = -1;
    const wchar_t *label = L"";    /* every branch below sets it */
    PlaceRequest req;
    PlaceReport rep;
    PlaceResult res;

    ZeroMemory(&launched, sizeof(launched));
    ZeroMemory(&info, sizeof(info));
    ZeroMemory(&remember, sizeof(remember));
    ZeroMemory(&cur, sizeof(cur));
    ZeroMemory(&mon, sizeof(mon));
    ZeroMemory(&target_rect, sizeof(target_rect));
    ZeroMemory(&req, sizeof(req));
    ZeroMemory(&rep, sizeof(rep));

    if (parse_place(argc, argv, &o) != 0) return 2;

    load_environment();

    /* ---- start, if asked ------------------------------------------ */

    if (o.launch_cmd[0] != L'\0') {
        if (!launch_start(&launched, o.launch_cmd)) {
            out(L"could not start: %ls\n", o.launch_cmd);
            return 1;
        }
        out(L"started %ls (pid %lu)\n",
            launched.exe_name[0] ? launched.exe_name : L"the program",
            (unsigned long)launched.pid);
    }

    /* ---- find the window ------------------------------------------ */

    if (o.sel.hwnd != NULL) {
        if (!win_alive(o.sel.hwnd)) {
            out(L"that window handle is no longer valid.\n");
            return 1;
        }
        target = o.sel.hwnd;
    } else if (launched.exe_name[0] != L'\0') {
        target = launch_wait(&launched, o.wait_ms,
                             o.verbose ? trace_to_console : NULL, NULL);
        if (target == NULL) {
            out(L"no window appeared within %d ms.\n", o.wait_ms);
            return 1;
        }
    } else {
        int n = win_enum_top(g_wins, WAC_WIN_MAX, WIN_ENUM_DEFAULT);
        int hits = 0;

        for (int i = 0; i < n; ++i) {
            if (app_id_match(&o.sel, &g_wins[i])) {
                if (hits == 0) {
                    target = g_wins[i].hwnd;
                    info = g_wins[i];
                }
                hits++;
            }
        }

        if (hits == 0) {
            out(L"no open window matches that rule.\n");
            out(L"run 'wac list' to see what is available, or use --launch.\n");
            return 1;
        }
        if (hits > 1) {
            out(L"note: %d windows match; using the first. Narrow it with --title or\n"
                L"      --class, or name one exactly with --hwnd.\n", hits);
        }
    }

    if (!win_fill_info(target, &info, WIN_ENUM_NEED_EXE)) {
        out(L"the window disappeared before it could be read.\n");
        return 1;
    }

    if (!GetWindowRect(target, &cur)) {
        out(L"GetWindowRect failed for the target window.\n");
        return 1;
    }

    /* ---- where does it go ----------------------------------------- */

    load_rules_reporting();

    mon_index = o.monitor_index;
    if (mon_index < 0) {
        mon_index = monitor_at_point(g_mons, g_mon_count,
                                     (int)((cur.left + cur.right) / 2),
                                     (int)((cur.top + cur.bottom) / 2));
        if (mon_index < 0) mon_index = monitor_primary_index(g_mons, g_mon_count);
    }

    if (!monitor_rect(g_mons, g_mon_count, mon_index, &mon)) {
        out(L"monitor %d is not available.\n", mon_index);
        return 1;
    }

    if (o.have_zone) {
        if (!zone_rect(o.zx, o.zy, o.zw, o.zh, &mon, &target_rect)) {
            out(L"that zone is not usable on monitor %d.\n", mon_index);
            return 1;
        }
        if (o.remember) {
            /*
             * A ZONE rule: the fractions are the whole answer, so abs_w/abs_h
             * stay zero and the window takes the PANEL's shape. That is what
             * "the left half" means - and it is the opposite of what
             * --remember without a zone records.
             */
            remember.x = o.zx;
            remember.y = o.zy;
            remember.w = o.zw;
            remember.h = o.zh;
            remember.min_w = 1;
            remember.min_h = 1;
            have_remember = 1;
        }
        label = L"--zone";
    } else {
        const Rule *r;

        rule_index = rules_match(&g_rules, &info);

        if (rule_index < 0 && o.remember) {
            /*
             * Nothing stored yet and --remember was given: the target IS
             * where the window already is. This is the only way to create a
             * remembered-size rule, because that kind records the window's
             * own pixel size - the user has to put the window where they want
             * it first, then say "remember this".
             */
            target_rect = cur;
            if (rel_from_pixels(&cur, &mon, &remember)) {
                remember.min_w = 0;
                remember.min_h = 0;
                have_remember = 1;
            }
            label = L"remembered position";
        } else if (rule_index < 0) {
            out(L"no --zone given, and no stored rule matches this window.\n");
            out(L"  place it once : wac place --app <name> --zone 0,0,0.5,1 --remember\n");
            out(L"  or see them   : wac rules\n");
            return 1;
        } else {
            r = rules_at(&g_rules, rule_index);

            /*
             * A stored preset may NAME a screen. That answer decides where the
             * window goes, and it beats "wherever the window currently is" -
             * the same rule the window's own placement paths follow
             * (2026-10-10). An explicit --monitor still wins over the stored
             * pin: what the user typed just now outranks what was stored.
             */
            if (o.monitor_index < 0) {
                int pinned = -1;
                if (rule_pinned_monitor(r, g_mons, g_mon_count,
                                        &pinned) == RULE_PIN_FOUND) {
                    mon_index = pinned;
                    if (!monitor_rect(g_mons, g_mon_count, mon_index, &mon)) {
                        out(L"monitor %d is not available.\n", mon_index);
                        return 1;
                    }
                    out(L"monitor : %d  (%ld,%ld)-(%ld,%ld)  [pinned]\n",
                        mon_index, (long)mon.left, (long)mon.top,
                        (long)mon.right, (long)mon.bottom);
                }
            }

            if (!rule_target_rect(r, &mon, &target_rect)) {
                out(L"rule %d produced no usable rectangle.\n", rule_index);
                return 1;
            }
            if (o.remember && rel_from_pixels(&cur, &mon, &remember)) {
                remember.min_w = 0;
                remember.min_h = 0;
                have_remember = 1;
            }
            label = L"stored rule";
        }
    }

    /* ---- place it -------------------------------------------------- */

    out(L"window  : 0x%p  %ls\n", (void *)target, info.title);
    out(L"monitor : %d  (%d,%d)-(%d,%d)\n", mon_index,
        (int)mon.left, (int)mon.top, (int)mon.right, (int)mon.bottom);
    out(L"target  : (%d,%d)-(%d,%d)  %dx%d   [%ls]\n",
        (int)target_rect.left, (int)target_rect.top,
        (int)target_rect.right, (int)target_rect.bottom,
        (int)(target_rect.right - target_rect.left),
        (int)(target_rect.bottom - target_rect.top), label);

    req.hwnd         = target;
    req.target       = target_rect;
    req.allow_resize = (o.move_only || (rule_index >= 0 &&
                                        rules_at(&g_rules, rule_index)->zone.lock_size))
                     ? FALSE : TRUE;

    res = win_place_apply(&req, &rep);

    out(L"result  : %ls   (pos error %d px, size error %d px)\n",
        win_place_result_name(res), rep.pos_error, rep.size_error);

    if (rep.fg_before != rep.fg_after) {
        out(L"warning : the foreground window changed during placement.\n");
    }

    switch (res) {
    case PLACE_MOVED_ONLY:
        out(L"note    : the window refuses to resize; it will only ever be moved.\n");
        break;
    case PLACE_DENIED:
        out(L"note    : Windows refused. The target most likely runs elevated,\n");
        out(L"          and this program does not. Run it elevated to place those.\n");
        break;
    case PLACE_SKIPPED_MINIMIZED:
        out(L"note    : the window is minimized and was deliberately left alone.\n");
        break;
    case PLACE_SKIPPED_FULLSCREEN:
        out(L"note    : exclusive full-screen window, left alone.\n");
        break;
    default:
        break;
    }

    /* ---- remember -------------------------------------------------- */

    if (o.remember && have_remember) {
        AppId app;

        if (!app_id_from_window(&info, &app)) {
            out(L"remember: this window's executable could not be read, so no rule\n");
            out(L"          could be built for it.\n");
        } else {
            int idx = rules_upsert(&g_rules, &app, &remember, NULL);

            if (idx < 0) {
                out(L"remember: the rule list is full (%d), or the app names nothing.\n",
                    RULE_MAX);
            } else if (!rules_save(&g_rules)) {
                out(L"remember: could not write the configuration file.\n");
            } else {
                out(L"\nsaved rule %d:\n", idx);
                print_rule(idx, rules_at(&g_rules, idx));
            }
        }
    }

    return win_place_result_ok(res) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void cmd_version(void)
{
    out(L"wac - window assistance controller\n");
    out(L"single-file build, no runtime dependencies\n");
}

int cli_run(int argc, wchar_t **argv)
{
    const wchar_t *verb;

    if (argv == NULL || argc < 2) {
        cli_usage();
        return 0;
    }

    verb = argv[1];

    if (eq(verb, L"help") || eq(verb, L"--help") || eq(verb, L"-h")) {
        cli_usage();
        return 0;
    }
    if (eq(verb, L"version") || eq(verb, L"--version") || eq(verb, L"-v")) {
        cmd_version();
        return 0;
    }
    if (eq(verb, L"list"))     return cmd_list(argc - 2, argv + 2);
    if (eq(verb, L"monitors")) { load_environment(); return cmd_monitors(); }
    if (eq(verb, L"rules"))    return cmd_rules(argc - 2, argv + 2);
    if (eq(verb, L"apply"))    { load_environment(); return cmd_apply(argc - 2, argv + 2); }
    if (eq(verb, L"place"))    return cmd_place(argc - 2, argv + 2);
    if (eq(verb, L"run"))      return cmd_run(argc - 2, argv + 2);
    if (eq(verb, L"close"))    return cmd_close(argc - 2, argv + 2);
    if (eq(verb, L"pick"))     { load_environment(); return cmd_pick(); }

    out(L"unknown command: %ls\n\n", verb);
    cli_usage();
    return 2;
}
