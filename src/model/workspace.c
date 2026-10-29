/*
 * workspace.c - workspace list and its conversion to and from the store.
 *
 * Section-naming convention (the whole persistence contract lives here):
 *
 *     [workspace:<id>]      one per workspace
 *         name=<display name>
 *         slot_count=<n>
 *
 *     [slot:<id>:<n>]       one per application, n being its POSITION
 *         preset=<rule id>      what program and where (see the header)
 *         order=<n>             0 = parallel, >0 = sequential
 *         app_path=             empty = the preset's executable
 *         launch_args=          extra command-line arguments
 *         launch_kind=          0 create / 1 shell / 2 chromium
 *         wait_timeout_ms=
 *         has_override=         1 = the x/y/w/h below win over the preset
 *         x= y= w= h= min_w= min_h= abs_w= abs_h= ref_w= lock_size=
 *         mon_device= mon_friendly= mon_w= mon_h= mon_x= mon_y=
 *                               the monitor this slot was recorded on. Written
 *                               ONLY when the slot is bound (stage 7), so a
 *                               configuration that never used the feature keeps
 *                               exactly the shape it had before
 *
 * Keeping this in one file means the rest of the program never builds a
 * section name by hand.
 *
 * Positions, not names: slot n is [slot:<id>:<n>], so removing one renumbers
 * the rest. ws_to_store() rewrites every slot of a workspace, which is what
 * makes that renumbering consistent instead of leaving a hole.
 */

#include "workspace.h"
#include "store.h"

#include "../util/strutil.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Basic operations                                                    */
/* ------------------------------------------------------------------ */

void ws_clear(WorkspaceSet *set)
{
    /*
     * NOT memset: the set is about 1.4 MB now that every workspace carries
     * sixteen slots, and touching all of it on every load would hand the whole
     * amount to the working set (TRAPS.md #17). Every entry point
     * that hands out a Workspace zeroes that one element first.
     */
    if (set) set->count = 0;
}

Workspace *ws_find(WorkspaceSet *set, const char *id)
{
    if (set == NULL || id == NULL) return NULL;
    for (int i = 0; i < set->count; ++i) {
        if (_stricmp(set->items[i].id, id) == 0) return &set->items[i];
    }
    return NULL;
}

const Workspace *ws_find_const(const WorkspaceSet *set, const char *id)
{
    return ws_find((WorkspaceSet *)set, id);
}

/*
 * Lowercase ASCII, digits, '-' and '_' only.
 *
 * The id ends up inside "[workspace:<id>]", so a bracket, '=' or newline
 * would corrupt the file. Non-ASCII bytes are dropped rather than
 * transliterated: a workspace named in Chinese gets an id like "ws1", which
 * is stable, and its real name is stored separately in `name`.
 */
static void sanitize_id(const char *name, char *out, size_t out_size)
{
    size_t n = 0;
    if (out_size == 0) return;

    for (const char *p = name; *p && n + 1 < out_size; ++p) {
        unsigned char c = (unsigned char)*p;
        char m = 0;

        if (c >= 'A' && c <= 'Z')      m = (char)(c - 'A' + 'a');
        else if (c >= 'a' && c <= 'z') m = (char)c;
        else if (c >= '0' && c <= '9') m = (char)c;
        else if (c == '-' || c == '_') m = (char)c;
        else if (c == ' ' || c == '\t') m = '-';

        if (m) out[n++] = m;
    }

    /* Trim leading/trailing '-' so the id is never just punctuation. */
    size_t start = 0;
    while (start < n && out[start] == '-') start++;
    while (n > start && out[n - 1] == '-') n--;

    if (start > 0 && n > start) memmove(out, out + start, n - start);
    n = (n > start) ? (n - start) : 0;

    out[n] = '\0';
}

void ws_make_id(const WorkspaceSet *set, const char *name,
                char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;

    sanitize_id(name ? name : "", out, out_size);
    if (out[0] == '\0') {
        strncpy_s(out, out_size, "workspace", _TRUNCATE);
    }

    /* Resolve collisions by appending -2, -3, ... */
    if (ws_find((WorkspaceSet *)set, out) == NULL) return;

    char base[WS_ID_MAX];
    strncpy_s(base, sizeof(base), out, _TRUNCATE);

    for (int n = 2; n < 1000; ++n) {
        char candidate[WS_ID_MAX];
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s-%d", base, n);
        if (ws_find((WorkspaceSet *)set, candidate) == NULL) {
            strncpy_s(out, out_size, candidate, _TRUNCATE);
            return;
        }
    }

    /* Pathological: fall back to a guaranteed-unique numeric id. */
    for (int n = 1; n < 10000; ++n) {
        char candidate[WS_ID_MAX];
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "ws%d", n);
        if (ws_find((WorkspaceSet *)set, candidate) == NULL) {
            strncpy_s(out, out_size, candidate, _TRUNCATE);
            return;
        }
    }
}

Workspace *ws_add(WorkspaceSet *set, const char *id, const char *name)
{
    if (set == NULL || name == NULL || name[0] == '\0') return NULL;
    if (set->count >= WS_MAX) return NULL;

    /* Refuse a duplicate id: the config file keys on it. */
    if (id && id[0] && ws_find(set, id)) return NULL;

    Workspace *w = &set->items[set->count];
    memset(w, 0, sizeof(*w));

    if (id && id[0]) {
        strncpy_s(w->id, sizeof(w->id), id, _TRUNCATE);
    } else {
        ws_make_id(set, name, w->id, sizeof(w->id));
    }
    strncpy_s(w->name, sizeof(w->name), name, _TRUNCATE);

    set->count++;
    return w;
}

BOOL ws_remove(WorkspaceSet *set, const char *id)
{
    if (set == NULL || id == NULL) return FALSE;

    for (int i = 0; i < set->count; ++i) {
        if (_stricmp(set->items[i].id, id) != 0) continue;

        for (int j = i; j + 1 < set->count; ++j) {
            set->items[j] = set->items[j + 1];
        }
        set->count--;
        return TRUE;
    }
    return FALSE;
}

BOOL ws_rename(WorkspaceSet *set, const char *id, const char *new_name)
{
    if (set == NULL || id == NULL || new_name == NULL) return FALSE;
    if (new_name[0] == '\0') return FALSE;

    Workspace *w = ws_find(set, id);
    if (w == NULL) return FALSE;

    /* The id is deliberately NOT regenerated: it is what the [slot:<id>:n]
     * sections are keyed on, so changing it would orphan every slot. */
    strncpy_s(w->name, sizeof(w->name), new_name, _TRUNCATE);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Slots                                                               */
/* ------------------------------------------------------------------ */

void ws_slot_defaults(WorkspaceSlot *s)
{
    if (s == NULL) return;

    memset(s, 0, sizeof(*s));

    /* The defaults are the "inherit / do nothing surprising" values: a slot
     * that has just been created must not be able to place a window by
     * accident, and it must not claim a monitor state it was never given. */
    s->wait_timeout_ms = WSS_WAIT_DEFAULT_MS;
    s->launch_kind     = LAUNCH_CREATE;
    s->state           = SLOT_READY;
    s->zone.min_w      = 1;
    s->zone.min_h      = 1;
}

WorkspaceSlot *ws_slot_add(Workspace *w)
{
    if (w == NULL || w->slot_count < 0) return NULL;
    if (w->slot_count >= WSS_SLOT_MAX) return NULL;

    WorkspaceSlot *s = &w->slots[w->slot_count];
    ws_slot_defaults(s);
    w->slot_count++;
    return s;
}

BOOL ws_slot_remove(Workspace *w, int index)
{
    if (w == NULL || index < 0 || index >= w->slot_count) return FALSE;

    for (int i = index; i + 1 < w->slot_count; ++i) {
        w->slots[i] = w->slots[i + 1];
    }
    w->slot_count--;

    /* Clear the vacated element rather than leaving a copy of the last slot
     * behind it. Nothing reads past slot_count today, and this is what keeps
     * that true after a later change forgets. */
    ws_slot_defaults(&w->slots[w->slot_count]);
    return TRUE;
}

BOOL ws_slot_effective(const WorkspaceSlot *slot, const Rule *preset, Rule *out)
{
    if (slot == NULL || out == NULL) return FALSE;

    ZeroMemory(out, sizeof(*out));

    if (preset != NULL) {
        /* Start from the preset: it is the answer to "what program, where",
         * and the slot is the list of exceptions to it. */
        *out = *preset;
        out->enabled = 1;      /* the slot exists because the user wants it */
    } else {
        /* A standalone slot. Give it the same defaults a fresh rule gets, so
         * an override that omits a field cannot mean "zero-sized window". */
        out->zone.min_w = 1;
        out->zone.min_h = 1;
        out->zone.w = 0.5f;
        out->zone.h = 1.0f;
        out->enabled = 1;
    }

    if (slot->has_override) {
        out->zone = slot->zone;
    }

    /*
     * A slot may name its own executable. When it does, the FILE NAME has to
     * be derived from it as well: the window search matches on the file name,
     * and a rule with only a path would look for a process called "".
     */
    if (slot->app_path[0] != L'\0') {
        wcsncpy_s(out->app.exe_path, WAC_EXE_MAX, slot->app_path, _TRUNCATE);
        wcsncpy_s(out->app.exe_name, WAC_EXE_MAX,
                  path_file_name_w(slot->app_path), _TRUNCATE);
    }

    /* Nothing to recognise the program by: the caller must say SKIPPED, not
     * "started and timed out". */
    if (out->app.exe_name[0] == L'\0' && out->app.exe_path[0] == L'\0') {
        return FALSE;
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Monitor binding (stage 7)                                           */
/* ------------------------------------------------------------------ */

void ws_slot_bind_monitor(WorkspaceSlot *s, const MonitorFingerprint *fp)
{
    if (s == NULL || fp == NULL) return;

    wcsncpy_s(s->mon_device,   WAC_MON_NAME_MAX,     fp->device_name,   _TRUNCATE);
    wcsncpy_s(s->mon_friendly, WAC_MON_FRIENDLY_MAX, fp->friendly_name, _TRUNCATE);
    s->mon_w = fp->width;
    s->mon_h = fp->height;
    s->mon_x = fp->virtual_x;
    s->mon_y = fp->virtual_y;

    /* Bound only when there is something to match on later. A fingerprint
     * carrying neither name cannot identify a monitor, and claiming it could
     * would leave the slot permanently ORPHANED - worse than leaving it
     * unbound, where it applies wherever the window turns up. */
    s->mon_set = (s->mon_device[0] != L'\0' || s->mon_friendly[0] != L'\0')
                     ? TRUE : FALSE;
}

/*
 * Human-readable state, English and untranslated on purpose: these strings go
 * into the log, and a log whose text changes with the UI language is a log
 * nobody can grep - the same rule the toolbar's button names follow.
 */
const wchar_t *ws_slot_state_name(SlotState st)
{
    switch (st) {
    case SLOT_READY:    return L"READY";
    case SLOT_ORPHANED: return L"ORPHANED";
    case SLOT_REBOUND:  return L"REBOUND";
    case SLOT_DEGRADED: return L"DEGRADED";
    default:            break;
    }
    return L"?";
}

SlotState ws_slot_evaluate_monitor(const WorkspaceSlot *s,
                                   const MonitorFingerprint *cur, int count,
                                   int *out_index)
{
    if (out_index != NULL) *out_index = -1;
    if (s == NULL) return SLOT_READY;

    /* Unbound: the pre-stage-7 behaviour. Every slot written by an older
     * build lands here, and it must keep meaning "apply it wherever the
     * window is". */
    if (!s->mon_set) return SLOT_READY;
    if (cur == NULL || count <= 0) return SLOT_ORPHANED;

    /*
     * Rebuild the fingerprint the slot remembers, in the shape monitor_match
     * takes. The struct is zeroed first so that the fields this module does
     * not store (dpi, is_primary) cannot leak a stale value into a comparison
     * that only ever looks at the three levels.
     */
    MonitorFingerprint target;
    ZeroMemory(&target, sizeof(target));
    wcsncpy_s(target.device_name,   WAC_MON_NAME_MAX,     s->mon_device,   _TRUNCATE);
    wcsncpy_s(target.friendly_name, WAC_MON_FRIENDLY_MAX, s->mon_friendly, _TRUNCATE);
    target.width     = s->mon_w;
    target.height    = s->mon_h;
    target.virtual_x = s->mon_x;
    target.virtual_y = s->mon_y;

    int idx = -1;
    const MonitorMatchLevel level = monitor_match(&target, cur, count, &idx);

    if (level == MONITOR_MATCH_NONE) return SLOT_ORPHANED;
    if (out_index != NULL) *out_index = idx;

    /* Geometry alone: the numbers line up, which is evidence but not proof -
     * two same-sized panels can be swapped without this noticing. Recorded as
     * REBOUND so the caller can say so instead of presenting it as certain. */
    if (level == MONITOR_MATCH_GEOMETRY) return SLOT_REBOUND;

    /*
     * EXACT or NAME: the same monitor. It is DEGRADED when the panel it is on
     * now is much narrower than the one it was recorded on, because a zone no
     * longer produces the sizes the user saw.
     *
     * The band is the one rel_resolve_size() uses for "this is a different
     * panel", shared on purpose (REL_JITTER_FRACTION) so the two features
     * cannot end up disagreeing about what that means.
     */
    if (s->mon_w > 0 && cur[idx].width > 0) {
        const float grow = (float)cur[idx].width / (float)s->mon_w;
        if (fabsf(grow - 1.0f) >= REL_JITTER_FRACTION) return SLOT_DEGRADED;
    }

    return SLOT_READY;
}

/* ------------------------------------------------------------------ */
/* Store bridge                                                        */
/* ------------------------------------------------------------------ */

#define PREFIX_WORKSPACE "workspace:"
#define PREFIX_SLOT      "slot:"

/*
 * "slot:coding:3" -> id "coding" and index 3.
 *
 * Strict on purpose: a hand-edited file with "slot:coding:3a" or a negative
 * number is not slot 3, and treating it as one would silently move a window
 * the user did not ask about.
 */
static BOOL slot_parse(const char *section_name,
                       char *id_out, size_t id_cch, int *out_index)
{
    const size_t plen = sizeof(PREFIX_SLOT) - 1;   /* "slot:" */

    if (section_name == NULL || id_out == NULL || id_cch == 0 || out_index == NULL) {
        return FALSE;
    }
    if (_strnicmp(section_name, PREFIX_SLOT, plen) != 0) return FALSE;

    const char *rest  = section_name + plen;
    const char *colon = strrchr(rest, ':');
    if (colon == NULL || colon == rest) return FALSE;

    const size_t idlen = (size_t)(colon - rest);
    if (idlen >= id_cch) return FALSE;
    memcpy(id_out, rest, idlen);
    id_out[idlen] = '\0';

    const char *digits = colon + 1;
    if (*digits == '\0') return FALSE;

    int n = 0;
    for (const char *p = digits; *p; ++p) {
        if (*p < '0' || *p > '9') return FALSE;
        if (n > 100000) return FALSE;      /* absurd: refuse, do not overflow */
        n = n * 10 + (*p - '0');
    }

    *out_index = n;
    return TRUE;
}

/* ---- one slot <-> one section ------------------------------------- */

static void slot_from_store(WorkspaceSlot *s, const void *store_doc,
                            const char *sec)
{
    const StoreDocument *doc = (const StoreDocument *)store_doc;

    ws_slot_defaults(s);

    strncpy_s(s->preset_id, sizeof(s->preset_id),
              store_get(doc, sec, "preset", ""), _TRUNCATE);

    s->has_override    = store_get_int(doc, sec, "has_override", 0) ? TRUE : FALSE;
    s->order           = store_get_int(doc, sec, "order", 0);
    s->wait_timeout_ms = store_get_int(doc, sec, "wait_timeout_ms",
                                       WSS_WAIT_DEFAULT_MS);

    /* An out-of-range value from a hand-edited file falls back to the
     * ordinary path rather than producing a launch kind nothing implements. */
    const int kind = store_get_int(doc, sec, "launch_kind", (int)LAUNCH_CREATE);
    s->launch_kind = (kind == (int)LAUNCH_SHELL || kind == (int)LAUNCH_CHROMIUM)
                         ? (LaunchKind)kind
                         : LAUNCH_CREATE;

    const char *path = store_get(doc, sec, "app_path", "");
    if (path[0] != '\0') str_to_wide(path, s->app_path, WSS_APP_PATH_MAX);

    const char *args = store_get(doc, sec, "launch_args", "");
    if (args[0] != '\0') str_to_wide(args, s->launch_args, WSS_ARGS_MAX);

    if (s->has_override) {
        s->zone.x = store_get_float(doc, sec, "x", 0.0f);
        s->zone.y = store_get_float(doc, sec, "y", 0.0f);
        s->zone.w = store_get_float(doc, sec, "w", 0.5f);
        s->zone.h = store_get_float(doc, sec, "h", 1.0f);

        s->zone.min_w  = store_get_int(doc, sec, "min_w", 1);
        s->zone.min_h  = store_get_int(doc, sec, "min_h", 1);
        s->zone.abs_w  = store_get_int(doc, sec, "abs_w", 0);
        s->zone.abs_h  = store_get_int(doc, sec, "abs_h", 0);
        s->zone.ref_w  = store_get_int(doc, sec, "ref_w", 0);
        s->zone.lock_size = store_get_int(doc, sec, "lock_size", 0) ? 1u : 0u;
    }

    /* ---- monitor binding (stage 7) ---------------------------------
     *
     * Absent from every file written before stage 7, which leaves mon_set
     * FALSE - the pre-stage-7 behaviour of "apply it wherever the window is".
     * A section that carries geometry but no name is treated as unbound too:
     * geometry alone cannot say WHICH screen this was, and a guess is worse
     * than no binding at all.
     */
    {
        const char *dev = store_get(doc, sec, "mon_device", "");
        if (dev[0] != '\0') str_to_wide(dev, s->mon_device, WAC_MON_NAME_MAX);

        const char *fr = store_get(doc, sec, "mon_friendly", "");
        if (fr[0] != '\0') str_to_wide(fr, s->mon_friendly, WAC_MON_FRIENDLY_MAX);

        s->mon_set = (s->mon_device[0] != L'\0' || s->mon_friendly[0] != L'\0')
                         ? TRUE : FALSE;

        if (s->mon_set) {
            s->mon_w = store_get_int(doc, sec, "mon_w", 0);
            s->mon_h = store_get_int(doc, sec, "mon_h", 0);
            s->mon_x = store_get_int(doc, sec, "mon_x", 0);
            s->mon_y = store_get_int(doc, sec, "mon_y", 0);
        }
    }
}

static void slot_to_store(void *store_doc, const char *sec,
                          const WorkspaceSlot *s)
{
    StoreDocument *doc = (StoreDocument *)store_doc;
    char num[32];

    store_set(doc, sec, "preset", s->preset_id);

    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->order);
    store_set(doc, sec, "order", num);

    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", (int)s->launch_kind);
    store_set(doc, sec, "launch_kind", num);

    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->wait_timeout_ms);
    store_set(doc, sec, "wait_timeout_ms", num);

    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->has_override ? 1 : 0);
    store_set(doc, sec, "has_override", num);

    /*
     * Wide strings go into the file as UTF-8, the same encoding the workspace
     * NAME already uses. A path that does not survive the conversion is stored
     * as empty, which the executor reports as NOT_FOUND - better than a
     * truncated path that launches the wrong program.
     */
    {
        char utf8[WSS_APP_PATH_MAX * 4];
        utf8[0] = '\0';
        if (s->app_path[0] != L'\0') wide_to_str(s->app_path, utf8, sizeof(utf8));
        store_set(doc, sec, "app_path", utf8);
    }
    {
        char utf8[WSS_ARGS_MAX * 4];
        utf8[0] = '\0';
        if (s->launch_args[0] != L'\0') wide_to_str(s->launch_args, utf8, sizeof(utf8));
        store_set(doc, sec, "launch_args", utf8);
    }

    if (s->has_override) {
        store_format_float(num, sizeof(num), s->zone.x); store_set(doc, sec, "x", num);
        store_format_float(num, sizeof(num), s->zone.y); store_set(doc, sec, "y", num);
        store_format_float(num, sizeof(num), s->zone.w); store_set(doc, sec, "w", num);
        store_format_float(num, sizeof(num), s->zone.h); store_set(doc, sec, "h", num);

        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->zone.min_w);
        store_set(doc, sec, "min_w", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->zone.min_h);
        store_set(doc, sec, "min_h", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->zone.abs_w);
        store_set(doc, sec, "abs_w", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->zone.abs_h);
        store_set(doc, sec, "abs_h", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->zone.ref_w);
        store_set(doc, sec, "ref_w", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->zone.lock_size ? 1 : 0);
        store_set(doc, sec, "lock_size", num);
    }

    /* ---- monitor binding (stage 7) ---------------------------------
     *
     * Written ONLY when the slot is bound, so a configuration that never used
     * the feature keeps exactly the shape it had before this change - which
     * is also what makes "the config file's bytes do not change when a
     * monitor is unplugged" (a stage-7 acceptance item) easy to hold: nothing
     * here runs unless a slot was already bound.
     */
    if (s->mon_set) {
        char utf8[WAC_MON_FRIENDLY_MAX * 4];

        utf8[0] = '\0';
        if (s->mon_device[0] != L'\0') wide_to_str(s->mon_device, utf8, sizeof(utf8));
        store_set(doc, sec, "mon_device", utf8);

        utf8[0] = '\0';
        if (s->mon_friendly[0] != L'\0') wide_to_str(s->mon_friendly, utf8, sizeof(utf8));
        store_set(doc, sec, "mon_friendly", utf8);

        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->mon_w);
        store_set(doc, sec, "mon_w", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->mon_h);
        store_set(doc, sec, "mon_h", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->mon_x);
        store_set(doc, sec, "mon_x", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", s->mon_y);
        store_set(doc, sec, "mon_y", num);
    }
}

/* "workspace:coding" -> "coding". Returns NULL when it is not a workspace
 * section or the id would be empty. */
static const char *workspace_id_of(const char *section_name)
{
    const size_t plen = sizeof(PREFIX_WORKSPACE) - 1;
    if (section_name == NULL) return NULL;
    if (_strnicmp(section_name, PREFIX_WORKSPACE, plen) != 0) return NULL;

    const char *id = section_name + plen;
    return (id[0] != '\0') ? id : NULL;
}

/*
 * Count [slot:<id>:<n>] sections for one workspace.
 *
 * Used when the workspace section has no slot_count - a hand-written config
 * may omit it, and deriving it is friendlier than reporting zero.
 */
static int count_slots(const void *store_doc, const char *ws_id)
{
    char prefix[WS_ID_MAX + 8];
    _snprintf_s(prefix, sizeof(prefix), _TRUNCATE, PREFIX_SLOT "%s:", ws_id);
    const size_t plen = strlen(prefix);

    int n = 0;
    const int sections = store_section_count((const StoreDocument *)store_doc);
    for (int i = 0; i < sections; ++i) {
        const char *name = store_section_name(store_section_at(store_doc, i));
        if (name && _strnicmp(name, prefix, plen) == 0) n++;
    }
    return n;
}

void ws_from_store(WorkspaceSet *set, const void *store_doc)
{
    ws_clear(set);
    if (store_doc == NULL) return;

    const int sections = store_section_count((const StoreDocument *)store_doc);

    for (int i = 0; i < sections && set->count < WS_MAX; ++i) {
        const char *name = store_section_name(store_section_at(store_doc, i));
        const char *id   = workspace_id_of(name);
        if (id == NULL) continue;

        Workspace *w = &set->items[set->count];
        memset(w, 0, sizeof(*w));
        strncpy_s(w->id, sizeof(w->id), id, _TRUNCATE);

        char sec[STORE_KEY_MAX];
        _snprintf_s(sec, sizeof(sec), _TRUNCATE, PREFIX_WORKSPACE "%s", id);

        const char *display = store_get((const StoreDocument *)store_doc,
                                        sec, "name", NULL);
        if (display && display[0]) {
            strncpy_s(w->name, sizeof(w->name), display, _TRUNCATE);
        } else {
            /* A section with no name still deserves an entry, otherwise the
             * user sees a workspace in the file that the window never
             * shows. Fall back to the id. */
            strncpy_s(w->name, sizeof(w->name), id, _TRUNCATE);
        }

        const char *sc = store_get((const StoreDocument *)store_doc,
                                   sec, "slot_count", NULL);
        if (sc && sc[0]) {
            w->slot_count = store_get_int((const StoreDocument *)store_doc,
                                          sec, "slot_count", 0);
            if (w->slot_count < 0) w->slot_count = 0;
            if (w->slot_count > WSS_SLOT_MAX) w->slot_count = WSS_SLOT_MAX;
        } else {
            w->slot_count = count_slots(store_doc, id);
        }

        set->count++;
    }

    /*
     * Pass 2: the slots. Each section names its own workspace and position, so
     * this walks the document rather than guessing at section names - and a
     * slot section whose workspace is not in the file is skipped.
     */
    {
        int seen[WS_MAX];
        for (int i = 0; i < set->count; ++i) seen[i] = 0;

        for (int i = 0; i < sections; ++i) {
            const char *sname = store_section_name(store_section_at(store_doc, i));

            char ws_id[WS_ID_MAX];
            int  n = 0;
            if (!slot_parse(sname, ws_id, sizeof(ws_id), &n)) continue;

            /* Past the maximum is skipped, not wrapped: a hand-edited
             * [slot:x:99] must not overwrite slot 3. */
            if (n < 0 || n >= WSS_SLOT_MAX) continue;

            const Workspace *found = ws_find_const(set, ws_id);
            if (found == NULL) continue;

            const int wi = (int)(found - set->items);
            slot_from_store(&set->items[wi].slots[n], store_doc, sname);
            if (n + 1 > seen[wi]) seen[wi] = n + 1;
        }

        /*
         * A file that lists more slots than it declares (or declares more than
         * it has) is taken at its word: whatever sections exist ARE the slots.
         * The alternative - trusting the count - would run a slot that has no
         * section behind it, which reads as "the workspace did nothing".
         */
        for (int i = 0; i < set->count; ++i) {
            if (seen[i] > set->items[i].slot_count) {
                set->items[i].slot_count = seen[i];
            }
        }
    }
}

void ws_to_store(const WorkspaceSet *set, void *store_doc)
{
    if (set == NULL || store_doc == NULL) return;

    StoreDocument *doc = (StoreDocument *)store_doc;

    /*
     * Pass 1: drop sections that should no longer exist.
     *
     * Workspace: its id is gone from the set. Slot: its workspace is gone, or
     * the workspace shrank past that position. Without the slot half, deleting
     * a workspace would leave its slots behind and they would come back on the
     * next load - the same failure the workspace half has always guarded.
     */
    const int sections = store_section_count(doc);
    for (int i = sections - 1; i >= 0; --i) {
        const char *name = store_section_name(store_section_at(doc, i));

        const char *ws_id = workspace_id_of(name);
        if (ws_id != NULL) {
            if (ws_find_const(set, ws_id) == NULL) store_remove_section(doc, name);
            continue;
        }

        char sid[WS_ID_MAX];
        int  n = 0;
        if (!slot_parse(name, sid, sizeof(sid), &n)) continue;

        const Workspace *w = ws_find_const(set, sid);
        if (w == NULL || n >= w->slot_count) {
            store_remove_section(doc, name);
        }
    }

    /* Pass 2: write every workspace in the set. */
    for (int i = 0; i < set->count; ++i) {
        const Workspace *w = &set->items[i];

        char sec[STORE_KEY_MAX];
        _snprintf_s(sec, sizeof(sec), _TRUNCATE, PREFIX_WORKSPACE "%s", w->id);

        store_set(doc, sec, "name", w->name);

        char num[16];
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", w->slot_count);
        store_set(doc, sec, "slot_count", num);

        /* Every slot is rewritten, which is what makes the "position" naming
         * consistent after one is removed in the middle. */
        for (int n = 0; n < w->slot_count && n < WSS_SLOT_MAX; ++n) {
            char ssec[STORE_KEY_MAX];
            _snprintf_s(ssec, sizeof(ssec), _TRUNCATE, PREFIX_SLOT "%s:%d", w->id, n);
            slot_to_store(doc, ssec, &w->slots[n]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

void ws_seed_defaults(WorkspaceSet *set)
{
    if (set == NULL) return;
    ws_clear(set);

    /*
     * One starter workspace rather than an empty window: the first thing a
     * user does after installing a layout tool is create a workspace, and a
     * list with a row already in it shows them the shape of the thing.
     *
     * name is stored as-is; the id is generated from it.
     */
    ws_add(set, NULL, "Default");
}
