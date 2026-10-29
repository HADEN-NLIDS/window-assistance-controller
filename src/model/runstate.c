/*
 * runstate.c - see runstate.h.
 */

#include "runstate.h"

#include "config.h"

#include "../util/strutil.h"

#include <stdio.h>
#include <stdlib.h>   /* strtoul */
#include <string.h>
#include <wchar.h>

#define RUNSTATE_FILE L"running.txt"
#define RUNSTATE_LINE 1200        /* id + pid + a full path, generously */

/* ------------------------------------------------------------------ */
/* The file                                                            */
/* ------------------------------------------------------------------ */

static BOOL runstate_path(wchar_t *out, size_t cch)
{
    wchar_t dir[MAX_PATH];
    if (out == NULL || cch == 0) return FALSE;
    if (!config_dir(dir, MAX_PATH)) return FALSE;

    return path_join(out, cch, dir, RUNSTATE_FILE);
}

/*
 * Read the whole file, calling `visit` for each usable line with the three
 * fields already split. Lines that do not have three fields are skipped: this
 * file is written by us and read by us, but a half-written line after a crash
 * must not stop the rest from loading.
 */
typedef void (*LineVisitor)(void *ctx, const char *ws, DWORD pid, const char *exe);

static BOOL runstate_walk(LineVisitor visit, void *ctx)
{
    wchar_t path[MAX_PATH];
    if (!runstate_path(path, MAX_PATH)) return FALSE;

    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == NULL) return FALSE;

    char line[RUNSTATE_LINE];
    while (fgets(line, sizeof(line), f) != NULL) {
        /* Strip the newline. */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        char *tab1 = strchr(line, '\t');
        if (tab1 == NULL) continue;
        *tab1 = '\0';

        char *tab2 = strchr(tab1 + 1, '\t');
        if (tab2 == NULL) continue;
        *tab2 = '\0';

        const DWORD pid = (DWORD)strtoul(tab1 + 1, NULL, 10);
        if (pid == 0) continue;

        visit(ctx, line, pid, tab2 + 1);
    }

    fclose(f);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Reading into a RunState                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    RunState *st;
    BOOL      found;
} LoadCtx;

static void load_visit(void *ctx, const char *ws, DWORD pid, const char *exe)
{
    LoadCtx *lc = (LoadCtx *)ctx;

    if (_stricmp(ws, lc->st->workspace_id) != 0) return;
    if (lc->st->count >= RUNSTATE_MAX) return;

    RunEntry *e = &lc->st->entries[lc->st->count];
    e->pid = pid;
    e->exe[0] = L'\0';
    str_to_wide(exe, e->exe, WAC_EXE_MAX);

    lc->st->count++;
    lc->found = TRUE;
}

void runstate_init(RunState *st, const char *workspace_id)
{
    if (st == NULL) return;

    ZeroMemory(st, sizeof(*st));
    if (workspace_id != NULL) {
        strncpy_s(st->workspace_id, sizeof(st->workspace_id), workspace_id, _TRUNCATE);
    }
}

/* The executable name of a live process, or FALSE. */
static BOOL exe_name_of(DWORD pid, wchar_t *out, size_t cch)
{
    if (out == NULL || cch == 0) return FALSE;
    out[0] = L'\0';

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) return FALSE;

    wchar_t path[MAX_PATH];
    DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
    const BOOL ok = QueryFullProcessImageNameW(h, 0, path, &len);
    CloseHandle(h);

    if (!ok) return FALSE;

    wcsncpy_s(out, cch, path_file_name_w(path), _TRUNCATE);
    return (out[0] != L'\0');
}

BOOL runstate_add(RunState *st, DWORD pid, const wchar_t *exe)
{
    if (st == NULL || pid == 0) return FALSE;
    if (st->count >= RUNSTATE_MAX) return FALSE;

    RunEntry *e = &st->entries[st->count];
    e->pid = pid;
    e->exe[0] = L'\0';

    if (exe != NULL && exe[0] != L'\0') {
        wcsncpy_s(e->exe, WAC_EXE_MAX, exe, _TRUNCATE);
    } else {
        /*
         * No name given: ask the process. That is the better source anyway -
         * it is exactly the string termination will compare against, so
         * recording it from anywhere else risks a mismatch that would leave
         * the program running.
         */
        exe_name_of(pid, e->exe, WAC_EXE_MAX);
    }

    if (e->exe[0] == L'\0') {
        /* Nothing to verify against later. Refusing is safer than recording
         * an entry that could only ever be "stale". */
        return FALSE;
    }

    st->count++;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *skip;      /* workspace whose lines are being replaced */
    const RunState *add;   /* what to append instead */
    FILE *out;
    BOOL  wrote_any;
} SaveCtx;

static void save_visit(void *ctx, const char *ws, DWORD pid, const char *exe)
{
    SaveCtx *sc = (SaveCtx *)ctx;

    /* Drop the lines of the workspace being rewritten; keep every other. */
    if (sc->skip != NULL && _stricmp(ws, sc->skip) == 0) return;

    fprintf(sc->out, "%s\t%lu\t%s\n", ws, (unsigned long)pid, exe);
    sc->wrote_any = TRUE;
}

BOOL runstate_save(const RunState *st)
{
    if (st == NULL || st->workspace_id[0] == '\0') return FALSE;

    wchar_t path[MAX_PATH];
    if (!runstate_path(path, MAX_PATH)) return FALSE;

    /*
     * Read the old file and write a new one beside it, then swap. The same
     * shape as the configuration writer, minus backups: losing this file
     * costs the ability to close a workspace automatically, nothing more.
     */
    wchar_t tmp[MAX_PATH];
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%ls.tmp", path);

    FILE *out = NULL;
    if (_wfopen_s(&out, tmp, L"wb") != 0 || out == NULL) return FALSE;

    SaveCtx sc;
    ZeroMemory(&sc, sizeof(sc));
    sc.skip = st->workspace_id;
    sc.out  = out;

    runstate_walk(save_visit, &sc);

    /* Append this workspace's new lines. */
    for (int i = 0; i < st->count; ++i) {
        char exe_utf8[WAC_EXE_MAX * 4];
        exe_utf8[0] = '\0';
        wide_to_str(st->entries[i].exe, exe_utf8, sizeof(exe_utf8));

        fprintf(out, "%s\t%lu\t%s\n", st->workspace_id,
                (unsigned long)st->entries[i].pid, exe_utf8);
    }

    fclose(out);

    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Loading and forgetting                                              */
/* ------------------------------------------------------------------ */

BOOL runstate_load(const char *workspace_id, RunState *out)
{
    if (workspace_id == NULL || out == NULL) return FALSE;

    runstate_init(out, workspace_id);

    LoadCtx lc;
    lc.st    = out;
    lc.found = FALSE;

    runstate_walk(load_visit, &lc);
    return lc.found;
}

typedef struct {
    const char *want;
    BOOL        removed;
} ForgetCtx;

static void forget_visit(void *ctx, const char *ws, DWORD pid, const char *exe)
{
    (void)pid; (void)exe;
    ForgetCtx *fc = (ForgetCtx *)ctx;
    if (_stricmp(ws, fc->want) == 0) fc->removed = TRUE;
}

BOOL runstate_forget(const char *workspace_id)
{
    if (workspace_id == NULL) return FALSE;

    ForgetCtx probe;
    probe.want    = workspace_id;
    probe.removed = FALSE;
    runstate_walk(forget_visit, &probe);
    if (!probe.removed) return FALSE;

    /* An empty RunState means "no lines for this workspace". */
    RunState empty;
    runstate_init(&empty, workspace_id);
    return runstate_save(&empty);
}

/* ------------------------------------------------------------------ */
/* Closing                                                             */
/* ------------------------------------------------------------------ */

int runstate_terminate(const RunState *st, RunStateTrace trace, void *ctx,
                       int *out_failed, int *out_stale)
{
    if (out_failed != NULL) *out_failed = 0;
    if (out_stale  != NULL) *out_stale  = 0;
    if (st == NULL) return 0;

    int ended = 0, failed = 0, stale = 0;

    for (int i = 0; i < st->count; ++i) {
        const DWORD pid = st->entries[i].pid;
        if (pid == 0) continue;

        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
        if (h == NULL) {
            /* Gone already, or not ours to touch. Either way: not a failure
             * the user has to act on, and definitely not something to retry. */
            stale++;
            if (trace != NULL) {
                wchar_t line[256];
                _snwprintf_s(line, 256, _TRUNCATE,
                             L"  pid %lu: no longer running", (unsigned long)pid);
                trace(ctx, line);
            }
            continue;
        }

        /*
         * THE CHECK THAT MATTERS. Between recording and closing, this pid may
         * have been recycled to an unrelated program. Compare the executable
         * before touching anything.
         */
        wchar_t now[MAX_PATH];
        const BOOL known = exe_name_of(pid, now, MAX_PATH);

        if (!known || _wcsicmp(now, st->entries[i].exe) != 0) {
            stale++;
            if (trace != NULL) {
                wchar_t line[320];
                _snwprintf_s(line, 320, _TRUNCATE,
                             L"  pid %lu: was \"%ls\", now \"%ls\" - left alone",
                             (unsigned long)pid, st->entries[i].exe,
                             known ? now : L"(unknown)");
                trace(ctx, line);
            }
            CloseHandle(h);
            continue;
        }

        const BOOL ok = TerminateProcess(h, 0);
        CloseHandle(h);

        if (ok) {
            ended++;
            if (trace != NULL) {
                wchar_t line[256];
                _snwprintf_s(line, 256, _TRUNCATE,
                             L"  pid %lu (%ls): ended",
                             (unsigned long)pid, st->entries[i].exe);
                trace(ctx, line);
            }
        } else {
            failed++;
            if (trace != NULL) {
                wchar_t line[256];
                _snwprintf_s(line, 256, _TRUNCATE,
                             L"  pid %lu (%ls): could not end (error %lu)",
                             (unsigned long)pid, st->entries[i].exe,
                             (unsigned long)GetLastError());
                trace(ctx, line);
            }
        }
    }

    if (out_failed != NULL) *out_failed = failed;
    if (out_stale  != NULL) *out_stale  = stale;
    return ended;
}
