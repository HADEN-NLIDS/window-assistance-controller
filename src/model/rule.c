/*
 * rule.c - see rule.h for the model and for why abs_w/abs_h decides which
 * kind of rule this is.
 *
 * This file has no Win32 window calls and does no I/O: it converts between a
 * RuleSet and a StoreDocument, and it computes rectangles. Everything that
 * touches a real window lives in win_place.c, and everything that touches a
 * real file lives in store.c. Keeping those apart is what makes the placement
 * rules testable without a screen.
 */

#include "rule.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "store.h"
#include "config.h"
#include "../util/strutil.h"

/* ------------------------------------------------------------------ */
/* Basics                                                              */
/* ------------------------------------------------------------------ */

void rules_clear(RuleSet *set)
{
    if (set == NULL) return;
    set->count = 0;
    /* The per-item contents are left alone on purpose: zeroing 70 KB every
     * time the list is rebuilt is pure cost, and `count` is what makes an
     * entry visible. */
}

int rules_count(const RuleSet *set)
{
    return (set == NULL) ? 0 : set->count;
}

const Rule *rules_at(const RuleSet *set, int index)
{
    if (set == NULL || index < 0 || index >= set->count) return NULL;
    return &set->items[index];
}

Rule *rules_at_mut(RuleSet *set, int index)
{
    if (set == NULL || index < 0 || index >= set->count) return NULL;
    return &set->items[index];
}

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

/*
 * Do two rules describe the same application?
 *
 * Full path when both have one, because two installs of "python.exe" are
 * different programs. Otherwise the file name, so a rule written as
 * "notepad.exe" still merges with one recorded from a full path.
 */
static BOOL same_app(const AppId *a, const AppId *b)
{
    if (a == NULL || b == NULL) return FALSE;

    if (a->exe_path[0] != L'\0' && b->exe_path[0] != L'\0') {
        return (_wcsicmp(a->exe_path, b->exe_path) == 0) ? TRUE : FALSE;
    }
    if (a->exe_name[0] != L'\0' && b->exe_name[0] != L'\0') {
        return (_wcsicmp(a->exe_name, b->exe_name) == 0) ? TRUE : FALSE;
    }
    return FALSE;
}

void rules_make_id(const RuleSet *set, const wchar_t *exe_name,
                   char *out, size_t out_size)
{
    char utf8[WAC_EXE_MAX * 2];
    char base[RULE_ID_MAX];
    int  used = 0;
    int  n;

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (exe_name == NULL || exe_name[0] == L'\0') {
        strncpy_s(out, out_size, "rule", _TRUNCATE);
        return;
    }

    utf8[0] = '\0';
    wide_to_str(path_file_name_w(exe_name), utf8, sizeof(utf8));

    /* Drop the extension: "notepad.exe" reads better as "notepad" and the
     * extension carries no information once the section is about one app. */
    char *dot = strrchr(utf8, '.');
    if (dot != NULL) *dot = '\0';

    /* Keep [a-z0-9_-] only, lowercasing as we go. Anything else would either
     * break the INI syntax or need escaping the format has no room for. */
    for (const char *p = utf8; *p != '\0' && used < RULE_ID_MAX - 4; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_') {
            base[used++] = c;
        }
    }
    base[used] = '\0';

    if (used == 0) {
        strncpy_s(base, RULE_ID_MAX, "rule", _TRUNCATE);
    }

    /* Uniqueness is checked against the ids already in the set, so two apps
     * that reduce to the same slug still get distinct sections. */
    for (n = 0; n < 1000; ++n) {
        if (n == 0) {
            strncpy_s(out, out_size, base, _TRUNCATE);
        } else {
            _snprintf_s(out, out_size, _TRUNCATE, "%s-%d", base, n);
        }

        int clash = 0;
        for (int i = 0; set != NULL && i < set->count; ++i) {
            /*
             * Skip the slot `out` points into.
             *
             * MEASURED: the first version compared against every entry,
             * including the caller's own destination buffer, so the freshly
             * written "notepad" collided with itself on every attempt. The
             * loop ran to its limit and the section came out as
             * "notepad-999" - which looks like a naming policy rather than
             * the bug it is.
             */
            if ((const char *)out == set->items[i].id) continue;
            if (_stricmp(set->items[i].id, out) == 0) { clash = 1; break; }
        }
        if (!clash) return;
    }
}

/* ------------------------------------------------------------------ */
/* Add / remove / find                                                 */
/* ------------------------------------------------------------------ */

int rules_upsert(RuleSet *set, const AppId *app, const RelRect *zone,
                 const wchar_t *label)
{
    int found = -1;

    if (set == NULL || app == NULL || zone == NULL) return -1;

    /* A rule that names nothing would be stored, listed, and never match
     * anything - app_id_match() treats an empty rule as matching nothing. */
    if (app_id_empty(app)) return -1;

    for (int i = 0; i < set->count; ++i) {
        if (same_app(&set->items[i].app, app)) { found = i; break; }
    }

    if (found < 0) {
        if (set->count >= RULE_MAX) return -1;
        found = set->count;
        ZeroMemory(&set->items[found], sizeof(Rule));
        set->items[found].app = *app;
        set->items[found].enabled = 1;
        set->items[found].label[0] = L'\0';

        wchar_t source[WAC_EXE_MAX];
        if (app->exe_name[0] != L'\0') {
            wcsncpy_s(source, WAC_EXE_MAX, app->exe_name, _TRUNCATE);
        } else {
            wcsncpy_s(source, WAC_EXE_MAX, app->exe_path, _TRUNCATE);
        }

        /* The id is generated while the entry is still OUTSIDE the set, so
         * the uniqueness check cannot see the slot it is writing into. */
        rules_make_id(set, source, set->items[found].id, RULE_ID_MAX);
        set->count++;
    }

    set->items[found].zone = *zone;

    if (label != NULL && label[0] != L'\0') {
        wcsncpy_s(set->items[found].label, RULE_LABEL_MAX, label, _TRUNCATE);
    }

    return found;
}

BOOL rules_remove(RuleSet *set, int index)
{
    if (set == NULL || index < 0 || index >= set->count) return FALSE;

    for (int i = index; i + 1 < set->count; ++i) {
        set->items[i] = set->items[i + 1];
    }
    set->count--;
    return TRUE;
}

int rules_find_by_name(const RuleSet *set, const wchar_t *name)
{
    char utf8[RULE_LABEL_MAX * 2];

    if (set == NULL || name == NULL || name[0] == L'\0') return -1;

    utf8[0] = '\0';
    wide_to_str(name, utf8, sizeof(utf8));

    for (int i = 0; i < set->count; ++i) {
        if (_stricmp(set->items[i].id, utf8) == 0) return i;
        if (set->items[i].label[0] != L'\0' &&
            _wcsicmp(set->items[i].label, name) == 0) {
            return i;
        }
        /* Also let the executable name find it, which is what a user types. */
        if (set->items[i].app.exe_name[0] != L'\0' &&
            _wcsicmp(set->items[i].app.exe_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int rules_match(const RuleSet *set, const WindowInfo *w)
{
    if (set == NULL || w == NULL) return -1;

    for (int i = 0; i < set->count; ++i) {
        if (!set->items[i].enabled) continue;
        if (app_id_match(&set->items[i].app, w)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Applying                                                            */
/* ------------------------------------------------------------------ */

BOOL rule_target_rect(const Rule *rule, const RECT *rc_monitor, RECT *out)
{
    POINT pt;
    SIZE  sz;
    int   w, h;

    if (rule == NULL || rc_monitor == NULL || out == NULL) return FALSE;
    if (rc_monitor->right <= rc_monitor->left) return FALSE;
    if (rc_monitor->bottom <= rc_monitor->top) return FALSE;

    /*
     * Position first, always, and always from the fractions. "Where on the
     * screen" is exactly what a normalized coordinate means.
     *
     * rel_to_pixels() also returns a size; it is discarded below when the
     * rule remembers one. Passing each fraction through its own axis is
     * correct FOR A ZONE, and a zone's position and size share the same
     * storage - so the position cannot be taken from anywhere else.
     */
    if (!rel_to_pixels(&rule->zone, rc_monitor, &pt, &sz)) return FALSE;

    w = sz.cx;
    h = sz.cy;

    if (rule->zone.abs_w > 0 && rule->zone.abs_h > 0) {
        /*
         * The rule remembers a size the user actually saw. Use it: a 16:9
         * window must stay 16:9 on a portrait panel, and rel_resolve_size()
         * is the function that guarantees that - it scales both axes by one
         * factor and shrinks only when the window no longer fits.
         */
        SIZE remembered;
        ZeroMemory(&remembered, sizeof(remembered));
        rel_resolve_size(&rule->zone, rc_monitor, &remembered);
        if (remembered.cx > 0 && remembered.cy > 0) {
            w = remembered.cx;
            h = remembered.cy;
        }
    }

    out->left   = pt.x;
    out->top    = pt.y;
    out->right  = pt.x + w;
    out->bottom = pt.y + h;

    /*
     * A remembered size is independent of the zone's fractions, so it can
     * push the window past the panel even though the fraction path clamped
     * itself. Pull it back rather than letting the user lose a title bar off
     * the edge of the screen.
     */
    if (out->right  > rc_monitor->right) {
        out->left  = rc_monitor->right - w;
        out->right = rc_monitor->right;
    }
    if (out->bottom > rc_monitor->bottom) {
        out->top    = rc_monitor->bottom - h;
        out->bottom = rc_monitor->bottom;
    }
    if (out->left < rc_monitor->left) out->left = rc_monitor->left;
    if (out->top  < rc_monitor->top)  out->top  = rc_monitor->top;

    return TRUE;
}

BOOL rule_lock_size(RuleSet *set, const char *rule_id)
{
    if (set == NULL || rule_id == NULL || rule_id[0] == '\0') return FALSE;

    for (int i = 0; i < set->count; ++i) {
        Rule *r = &set->items[i];
        if (_stricmp(r->id, rule_id) != 0) continue;

        /* Already known: nothing changed, so nothing to save or announce. */
        if (r->zone.lock_size) return FALSE;

        r->zone.lock_size = 1u;
        return TRUE;
    }

    /* No such preset. Not an error worth reporting: the sweep may be working
     * from a list that a refresh has already replaced. */
    return FALSE;
}

BOOL rule_is_pinned(const Rule *r)
{
    return (r != NULL && r->mon_set) ? TRUE : FALSE;
}

BOOL rule_note_size(RuleSet *set, const char *rule_id, int w, int h)
{
    if (set == NULL || rule_id == NULL || rule_id[0] == '\0') return FALSE;

    /* A degenerate measurement is not a measurement. Refused rather than
     * stored, because the placeholder box would draw it. */
    if (w <= 0 || h <= 0) return FALSE;

    for (int i = 0; i < set->count; ++i) {
        Rule *r = &set->items[i];
        if (_stricmp(r->id, rule_id) != 0) continue;

        /* Unchanged: nothing to save and nothing to announce, the same rule
         * rule_lock_size() follows. */
        if (r->last_w == w && r->last_h == h) return FALSE;

        r->last_w = w;
        r->last_h = h;
        return TRUE;
    }

    /* No such preset - the sweep may be working from a list a refresh has
     * already replaced. Not worth reporting. */
    return FALSE;
}

void rule_bind_monitor(Rule *r, const MonitorFingerprint *fp)
{
    if (r == NULL) return;

    if (fp == NULL) {
        r->mon_set = FALSE;
        r->mon_device[0]   = L'\0';
        r->mon_friendly[0] = L'\0';
        r->mon_w = r->mon_h = 0;
        r->mon_x = r->mon_y = 0;
        return;
    }

    r->mon_set = TRUE;
    wcsncpy_s(r->mon_device,   WAC_MON_NAME_MAX,     fp->device_name,   _TRUNCATE);
    wcsncpy_s(r->mon_friendly, WAC_MON_FRIENDLY_MAX, fp->friendly_name, _TRUNCATE);
    r->mon_w = fp->width;
    r->mon_h = fp->height;
    r->mon_x = fp->virtual_x;
    r->mon_y = fp->virtual_y;
}

RulePinState rule_pinned_monitor(const Rule *r, const MonitorFingerprint *cur,
                                 int count, int *out_index)
{
    if (out_index != NULL) *out_index = -1;
    if (!rule_is_pinned(r)) return RULE_PIN_NONE;
    if (cur == NULL || count <= 0) return RULE_PIN_GONE;

    /* Rebuilt in the shape monitor_match() takes. Zeroed first so the fields
     * this struct does not store (dpi, is_primary) cannot leak a stale value
     * into a comparison that only ever looks at the three levels - the same
     * shape ws_slot_evaluate_monitor() uses. */
    MonitorFingerprint target;
    ZeroMemory(&target, sizeof(target));
    wcsncpy_s(target.device_name,   WAC_MON_NAME_MAX,     r->mon_device,   _TRUNCATE);
    wcsncpy_s(target.friendly_name, WAC_MON_FRIENDLY_MAX, r->mon_friendly, _TRUNCATE);
    target.width     = r->mon_w;
    target.height    = r->mon_h;
    target.virtual_x = r->mon_x;
    target.virtual_y = r->mon_y;

    int idx = -1;
    if (monitor_match(&target, cur, count, &idx) == MONITOR_MATCH_NONE) {
        return RULE_PIN_GONE;
    }

    if (out_index != NULL) *out_index = idx;
    return RULE_PIN_FOUND;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

#define RULE_SECTION_PREFIX "rule:"

static BOOL is_rule_section(const char *name)
{
    if (name == NULL) return FALSE;
    return (strncmp(name, RULE_SECTION_PREFIX, 5) == 0) ? TRUE : FALSE;
}

/*
 * Load one section into a Rule.
 *
 * Returns FALSE when the section does not describe anything that could ever
 * match a window - an executable, a class or a title. Such a section is
 * skipped rather than loaded, because app_id_match() would never fire on it
 * and the user would be left with a rule that is visible in the file and
 * invisible in the behaviour.
 */
static BOOL load_one(const StoreDocument *doc, const char *section, Rule *r)
{
    char tmp[STORE_VALUE_MAX];

    ZeroMemory(r, sizeof(*r));

    /* The id is the section name minus its prefix, kept as bytes: it goes
     * straight back into a section name when saving. */
    strncpy_s(r->id, RULE_ID_MAX, section + 5, _TRUNCATE);

    tmp[0] = '\0';
    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "exe", ""), _TRUNCATE);
    str_to_wide(tmp, r->app.exe_name, WAC_EXE_MAX);

    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "exe_path", ""), _TRUNCATE);
    str_to_wide(tmp, r->app.exe_path, WAC_EXE_MAX);

    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "class", ""), _TRUNCATE);
    str_to_wide(tmp, r->app.class_name, WAC_CLASS_MAX);

    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "title", ""), _TRUNCATE);
    str_to_wide(tmp, r->app.title_sub, WAC_TITLE_MAX);

    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "label", ""), _TRUNCATE);
    str_to_wide(tmp, r->label, RULE_LABEL_MAX);

    if (r->app.exe_name[0] == L'\0' && r->app.exe_path[0] == L'\0' &&
        r->app.class_name[0] == L'\0' && r->app.title_sub[0] == L'\0') {
        return FALSE;
    }

    r->zone.x = store_get_float(doc, section, "x", 0.0f);
    r->zone.y = store_get_float(doc, section, "y", 0.0f);
    r->zone.w = store_get_float(doc, section, "w", 0.5f);
    r->zone.h = store_get_float(doc, section, "h", 1.0f);

    r->zone.min_w = store_get_int(doc, section, "min_w", 1);
    r->zone.min_h = store_get_int(doc, section, "min_h", 1);
    r->zone.abs_w = store_get_int(doc, section, "abs_w", 0);
    r->zone.abs_h = store_get_int(doc, section, "abs_h", 0);
    r->zone.ref_w = store_get_int(doc, section, "ref_w", 0);

    r->zone.lock_size = store_get_int(doc, section, "lock_size", 0) ? 1u : 0u;
    r->enabled        = store_get_int(doc, section, "enabled", 1);

    /* The last measured size, or 0 - the state of every preset written before
     * 2026-10-10, which is exactly what "never placed yet" means. */
    r->last_w = store_get_int(doc, section, "last_w", 0);
    r->last_h = store_get_int(doc, section, "last_h", 0);

    /* The pinned screen, if the file names one. A section written before
     * 2026-10-10 has none of these keys, stays mon_set == FALSE, and keeps
     * meaning "wherever the window turns up". */
    tmp[0] = '\0';
    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "mon_device", ""), _TRUNCATE);
    str_to_wide(tmp, r->mon_device, WAC_MON_NAME_MAX);

    tmp[0] = '\0';
    strncpy_s(tmp, sizeof(tmp), store_get(doc, section, "mon_friendly", ""), _TRUNCATE);
    str_to_wide(tmp, r->mon_friendly, WAC_MON_FRIENDLY_MAX);

    r->mon_w = store_get_int(doc, section, "mon_w", 0);
    r->mon_h = store_get_int(doc, section, "mon_h", 0);
    r->mon_x = store_get_int(doc, section, "mon_x", 0);
    r->mon_y = store_get_int(doc, section, "mon_y", 0);

    r->mon_set = (r->mon_device[0] != L'\0' || r->mon_friendly[0] != L'\0')
                     ? TRUE : FALSE;

    /* Clamp what a hand-edited file could have made nonsense. A zone outside
     * [0,1] or a negative minimum must not reach SetWindowPos. */
    if (r->zone.w < 0.0f) r->zone.w = 0.0f;
    if (r->zone.h < 0.0f) r->zone.h = 0.0f;
    if (r->zone.w > 1.0f) r->zone.w = 1.0f;
    if (r->zone.h > 1.0f) r->zone.h = 1.0f;
    if (r->zone.x < 0.0f) r->zone.x = 0.0f;
    if (r->zone.y < 0.0f) r->zone.y = 0.0f;
    if (r->zone.min_w < 0) r->zone.min_w = 0;
    if (r->zone.min_h < 0) r->zone.min_h = 0;
    if (r->zone.abs_w < 0) r->zone.abs_w = 0;
    if (r->zone.abs_h < 0) r->zone.abs_h = 0;
    if (r->zone.ref_w < 0) r->zone.ref_w = 0;

    /* A negative "last seen" size is not a size; 0 is the honest "unknown". */
    if (r->last_w < 0) r->last_w = 0;
    if (r->last_h < 0) r->last_h = 0;

    return TRUE;
}

void rules_from_store(RuleSet *set, const void *store_doc)
{
    const StoreDocument *doc = (const StoreDocument *)store_doc;

    if (set == NULL) return;
    rules_clear(set);
    if (doc == NULL) return;

    int n = store_section_count(doc);

    for (int i = 0; i < n && set->count < RULE_MAX; ++i) {
        const void *sec = store_section_at(doc, i);
        const char *name = store_section_name(sec);

        if (!is_rule_section(name)) continue;
        if (!load_one(doc, name, &set->items[set->count])) continue;

        set->count++;
    }
}

void rules_to_store(const RuleSet *set, void *store_doc)
{
    StoreDocument *doc = (StoreDocument *)store_doc;
    char section[RULE_ID_MAX + 16];
    char tmp[STORE_VALUE_MAX];
    char num[64];

    if (doc == NULL || set == NULL) return;

    /*
     * Delete first, walking BACKWARDS.
     *
     * store_remove_section() shifts the remaining sections down, so a forward
     * loop would skip every other one. Walking backwards also means the
     * sections to be rewritten are removed before any is written, which is
     * what makes a rule deleted in the UI actually disappear from the file
     * instead of being re-added by a stale section.
     */
    for (int i = store_section_count(doc) - 1; i >= 0; --i) {
        const void *sec = store_section_at(doc, i);
        const char *name = store_section_name(sec);
        if (is_rule_section(name)) store_remove_section(doc, name);
    }

    for (int i = 0; i < set->count; ++i) {
        const Rule *r = &set->items[i];

        _snprintf_s(section, sizeof(section), _TRUNCATE,
                    RULE_SECTION_PREFIX "%s", r->id);

        tmp[0] = '\0';
        wide_to_str(r->app.exe_name, tmp, sizeof(tmp));
        store_set(doc, section, "exe", tmp);

        tmp[0] = '\0';
        wide_to_str(r->app.exe_path, tmp, sizeof(tmp));
        store_set(doc, section, "exe_path", tmp);

        tmp[0] = '\0';
        wide_to_str(r->app.class_name, tmp, sizeof(tmp));
        store_set(doc, section, "class", tmp);

        tmp[0] = '\0';
        wide_to_str(r->app.title_sub, tmp, sizeof(tmp));
        store_set(doc, section, "title", tmp);

        tmp[0] = '\0';
        wide_to_str(r->label, tmp, sizeof(tmp));
        store_set(doc, section, "label", tmp);

        store_format_float(num, sizeof(num), r->zone.x);
        store_set(doc, section, "x", num);
        store_format_float(num, sizeof(num), r->zone.y);
        store_set(doc, section, "y", num);
        store_format_float(num, sizeof(num), r->zone.w);
        store_set(doc, section, "w", num);
        store_format_float(num, sizeof(num), r->zone.h);
        store_set(doc, section, "h", num);

        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->zone.min_w);
        store_set(doc, section, "min_w", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->zone.min_h);
        store_set(doc, section, "min_h", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->zone.abs_w);
        store_set(doc, section, "abs_w", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->zone.abs_h);
        store_set(doc, section, "abs_h", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->zone.ref_w);
        store_set(doc, section, "ref_w", num);

        store_set(doc, section, "lock_size", r->zone.lock_size ? "1" : "0");
        store_set(doc, section, "enabled", r->enabled ? "1" : "0");

        /*
         * The last measured size, written only once there IS one. A preset that
         * has never been placed keeps exactly the section it had, the same rule
         * the monitor keys below follow - so a file written by an older build
         * does not grow two zero-valued keys just by being loaded and saved.
         */
        if (r->last_w > 0 && r->last_h > 0) {
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->last_w);
            store_set(doc, section, "last_w", num);
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->last_h);
            store_set(doc, section, "last_h", num);
        }

        /* Written only when the preset is pinned, so an unbound preset keeps
         * exactly the section it had - and a hand-edited file that never had
         * these keys is not rewritten with empty ones. */
        if (r->mon_set) {
            tmp[0] = '\0';
            wide_to_str(r->mon_device, tmp, sizeof(tmp));
            store_set(doc, section, "mon_device", tmp);

            tmp[0] = '\0';
            wide_to_str(r->mon_friendly, tmp, sizeof(tmp));
            store_set(doc, section, "mon_friendly", tmp);

            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->mon_w);
            store_set(doc, section, "mon_w", num);
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->mon_h);
            store_set(doc, section, "mon_h", num);
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->mon_x);
            store_set(doc, section, "mon_x", num);
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", r->mon_y);
            store_set(doc, section, "mon_y", num);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The standard rule file                                              */
/* ------------------------------------------------------------------ */

RulesLoadResult rules_load(RuleSet *set)
{
    wchar_t path[MAX_PATH];
    StoreDocument *doc = NULL;
    StoreLoadResult lr;

    if (set == NULL) return RULES_UNAVAILABLE;
    rules_clear(set);

    if (!config_file(path, MAX_PATH)) return RULES_UNAVAILABLE;

    lr = store_load(path, &doc);

    if (doc == NULL) {
        return (lr == STORE_MISSING) ? RULES_MISSING : RULES_UNAVAILABLE;
    }

    rules_from_store(set, doc);
    store_destroy(doc);

    if (lr == STORE_RECOVERED_FROM_BACKUP) return RULES_RECOVERED;
    if (lr == STORE_MODIFIED_EXTERNALLY)   return RULES_MODIFIED;
    return RULES_LOADED;
}

BOOL rules_save(const RuleSet *set)
{
    wchar_t path[MAX_PATH];
    StoreDocument *doc = NULL;
    BOOL ok;

    if (set == NULL) return FALSE;
    if (!config_file(path, MAX_PATH)) return FALSE;

    /*
     * Load first, THEN modify - the shared-file discipline described in
     * store.h. Building a fresh document and saving it would delete every
     * [workspace:*] and [settings] section the main window owns.
     *
     * The mirror image of that mistake - the main window deleting [rule:*] -
     * is what actually happened: save_all() built a fresh document, and since
     * it runs on exit, every window preset was erased by a normal close.
     * Measured 2026-09-23. Both writers now go through the same call.
     */
    doc = store_open_for_write(path);
    if (doc == NULL) return FALSE;

    rules_to_store(set, doc);
    ok = store_save(path, doc);
    store_destroy(doc);
    return ok;
}
