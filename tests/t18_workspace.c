/*
 * t18_workspace.c - unit tests for src/model/workspace.c (stage 5: slots)
 *
 * WHAT IS WORTH TESTING HERE
 *
 *   The slot model is where a workspace stops being a name and starts being a
 *   set of instructions for launching programs. Two things can go quietly
 *   wrong, and both of them look like "the workspace did nothing":
 *
 *     1. THE POSITION NAMING. Slots are stored as [slot:<id>:<n>] where n is
 *        the POSITION, so removing one renumbers the rest. A stale section
 *        left behind at the old position would resurrect a slot the user
 *        deleted.
 *
 *     2. THE REFERENCE. A slot that refers to a window preset must come back
 *        with that reference intact - and with the override flag deciding
 *        whether its own coordinates mean anything. Losing the reference turns
 *        "open notepad on the left half" into "open nothing".
 *
 * SIZING WARNING (TRAPS.md #11)
 *
 *   A WorkspaceSet is about 1.4 MB. Every one below is `static` on purpose: a
 *   local one overflows the stack and the program dies with 0xC00000FD and no
 *   output at all.
 *
 * BUILD / RUN
 *   tests\build_tests.cmd t18_workspace
 *   tests\out\t18_workspace.exe --no-pause
 */

#include "test_common.h"

#include "../src/model/workspace.h"
#include "../src/model/store.h"

static int g_pass, g_fail;

static void ok(const wchar_t *what, int cond)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls", what);
    }
}

static void ok_int(const wchar_t *what, int got, int want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got %d, want %d)", what, got, want);
    }
}

static void ok_str(const wchar_t *what, const char *got, const char *want)
{
    const char *g = (got != NULL) ? got : "(null)";
    if (want != NULL && strcmp(g, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got \"%hs\", want \"%hs\")",
                   what, g, (want != NULL) ? want : "(null)");
    }
}

static void ok_wstr(const wchar_t *what, const wchar_t *got, const wchar_t *want)
{
    const wchar_t *g = (got != NULL) ? got : L"(null)";
    if (want != NULL && wcscmp(g, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        t_out_line(L"  FAIL: %ls  (got \"%ls\", want \"%ls\")",
                   what, g, (want != NULL) ? want : L"(null)");
    }
}

/* 1.4 MB - see the sizing warning at the top of this file. */
static WorkspaceSet g_set_a;
static WorkspaceSet g_set_b;

/* ------------------------------------------------------------------ */

static void test_defaults(void)
{
    T_SEC(L"slot defaults");

    WorkspaceSlot s;
    ws_slot_defaults(&s);

    ok_str(L"no preset id", s.preset_id, "");
    ok(L"no override", !s.has_override);
    ok_int(L"launch kind is CREATE", (int)s.launch_kind, (int)LAUNCH_CREATE);
    ok_int(L"default wait timeout", s.wait_timeout_ms, WSS_WAIT_DEFAULT_MS);
    ok_int(L"parallel by default", s.order, 0);
    ok_int(L"state starts READY", (int)s.state, (int)SLOT_READY);
    ok_int(L"min width floor is 1", s.zone.min_w, 1);
    ok_int(L"min height floor is 1", s.zone.min_h, 1);
    ok_wstr(L"no launch path", s.app_path, L"");
}

static void test_add_remove(void)
{
    T_SEC(L"adding and removing slots");

    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "coding", "Coding");
    ok(L"workspace created", w != NULL);
    if (w == NULL) return;

    ok_int(L"starts empty", w->slot_count, 0);
    ok_int(L"a fresh workspace has no slots to walk", w->slots[0].order, 0);

    /* Fill it to the limit. */
    for (int i = 0; i < WSS_SLOT_MAX; ++i) {
        WorkspaceSlot *s = ws_slot_add(w);
        if (s == NULL) break;
        s->order = i + 1;
    }
    ok_int(L"filled to the maximum", w->slot_count, WSS_SLOT_MAX);
    ok(L"one more is refused", ws_slot_add(w) == NULL);
    ok_int(L"count unchanged after the refusal", w->slot_count, WSS_SLOT_MAX);

    /* Remove one from the middle: everything after it moves down. */
    ok(L"remove slot 3", ws_slot_remove(w, 3));
    ok_int(L"count dropped", w->slot_count, WSS_SLOT_MAX - 1);
    ok_int(L"slot 3 is now the old slot 4", w->slots[3].order, 5);
    ok_int(L"the last slot moved down too", w->slots[WSS_SLOT_MAX - 2].order,
           WSS_SLOT_MAX);

    ok(L"negative index refused", !ws_slot_remove(w, -1));
    ok(L"past-the-end index refused", !ws_slot_remove(w, w->slot_count));
    ok_int(L"count still correct", w->slot_count, WSS_SLOT_MAX - 1);
}

static void test_round_trip(void)
{
    T_SEC(L"round trip through the store");

    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "coding", "编码工作区");
    if (w == NULL) { ok(L"workspace created", 0); return; }

    /* Slot 0: refers to a preset, no override - the ordinary case. */
    WorkspaceSlot *s0 = ws_slot_add(w);
    strncpy_s(s0->preset_id, sizeof(s0->preset_id), "notepad", _TRUNCATE);
    s0->order = 1;
    s0->wait_timeout_ms = 8000;

    /* Slot 1: standalone, with its own path, arguments and a zone override. */
    WorkspaceSlot *s1 = ws_slot_add(w);
    wcsncpy_s(s1->app_path, WSS_APP_PATH_MAX, L"C:\\Tools\\编辑器.exe", _TRUNCATE);
    wcsncpy_s(s1->launch_args, WSS_ARGS_MAX, L"--left --new", _TRUNCATE);
    s1->launch_kind = LAUNCH_SHELL;
    s1->has_override = TRUE;
    s1->zone.x = 0.25f; s1->zone.y = 0.0f;
    s1->zone.w = 0.5f;  s1->zone.h = 1.0f;
    s1->zone.min_w = 320; s1->zone.min_h = 200;
    s1->zone.abs_w = 1280; s1->zone.abs_h = 1600; s1->zone.ref_w = 2560;
    s1->zone.lock_size = 1;

    StoreDocument *doc = store_create();
    ok(L"document created", doc != NULL);
    if (doc == NULL) return;

    ws_to_store(&g_set_a, doc);
    ws_clear(&g_set_b);
    ws_from_store(&g_set_b, doc);
    store_destroy(doc);

    ok_int(L"one workspace came back", g_set_b.count, 1);
    if (g_set_b.count != 1) return;

    const Workspace *r = &g_set_b.items[0];
    ok_str(L"id survived", r->id, "coding");
    ok_str(L"name survived (UTF-8)", r->name, "编码工作区");
    ok_int(L"two slots came back", r->slot_count, 2);

    ok_str(L"slot 0 keeps its preset reference", r->slots[0].preset_id, "notepad");
    ok(L"slot 0 has no override", !r->slots[0].has_override);
    ok_int(L"slot 0 order", r->slots[0].order, 1);
    ok_int(L"slot 0 timeout", r->slots[0].wait_timeout_ms, 8000);
    ok_int(L"slot 0 launch kind defaults to CREATE",
           (int)r->slots[0].launch_kind, (int)LAUNCH_CREATE);

    ok_str(L"slot 1 has no preset reference", r->slots[1].preset_id, "");
    ok_int(L"slot 1 launch kind", (int)r->slots[1].launch_kind, (int)LAUNCH_SHELL);
    ok_wstr(L"slot 1 path survived (UTF-8 path)", r->slots[1].app_path,
            L"C:\\Tools\\编辑器.exe");
    ok_wstr(L"slot 1 arguments survived", r->slots[1].launch_args, L"--left --new");

    ok(L"slot 1 override flag survived", r->slots[1].has_override);
    ok(L"slot 1 zone.x", r->slots[1].zone.x > 0.2499f && r->slots[1].zone.x < 0.2501f);
    ok(L"slot 1 zone.w", r->slots[1].zone.w > 0.4999f && r->slots[1].zone.w < 0.5001f);
    ok_int(L"slot 1 min_w", r->slots[1].zone.min_w, 320);
    ok_int(L"slot 1 abs_h", r->slots[1].zone.abs_h, 1600);
    ok_int(L"slot 1 ref_w", r->slots[1].zone.ref_w, 2560);
    ok_int(L"slot 1 lock_size", (int)r->slots[1].zone.lock_size, 1);
}

static void test_override_is_not_written_when_off(void)
{
    T_SEC(L"an unset override stays unset");

    /*
     * has_override = 0 means "everything comes from the preset". Writing the
     * zone anyway would be a second source of truth: the executor would have
     * to know which of the two to believe.
     */
    StoreDocument *doc = store_create();
    if (doc == NULL) return;

    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "plain", "Plain");
    WorkspaceSlot *s = ws_slot_add(w);
    strncpy_s(s->preset_id, sizeof(s->preset_id), "notepad", _TRUNCATE);
    s->zone.w = 0.75f;              /* set, but has_override is FALSE */
    s->has_override = FALSE;

    ws_to_store(&g_set_a, doc);
    ok(L"the zone is NOT written", store_get(doc, "slot:plain:0", "x", NULL) == NULL);
    ok_str(L"has_override is 0", store_get(doc, "slot:plain:0", "has_override", "?"), "0");
    ok_str(L"preset is there", store_get(doc, "slot:plain:0", "preset", "?"), "notepad");

    store_destroy(doc);
}

static void test_removed_workspace_takes_its_slots(void)
{
    T_SEC(L"deleting a workspace deletes its slots");

    StoreDocument *doc = store_create();
    if (doc == NULL) return;

    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "temp", "Temp");
    ws_slot_add(w);
    ws_slot_add(w);

    ws_to_store(&g_set_a, doc);
    ok(L"slot section was written",
       store_get(doc, "slot:temp:0", "preset", NULL) != NULL ||
       store_get(doc, "slot:temp:0", "has_override", NULL) != NULL);

    ok(L"workspace removed", ws_remove(&g_set_a, "temp"));
    ws_to_store(&g_set_a, doc);

    ok_int(L"no workspace section left",
           store_section_count(doc), 0);

    /* Reload into a fresh set: nothing may come back. */
    ws_clear(&g_set_b);
    ws_from_store(&g_set_b, doc);
    ok_int(L"reload finds no workspace", g_set_b.count, 0);

    store_destroy(doc);
}

static void test_shrinking_removes_trailing_slot_sections(void)
{
    T_SEC(L"shrinking a workspace drops the trailing slot sections");

    StoreDocument *doc = store_create();
    if (doc == NULL) return;

    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "ws", "WS");
    ws_slot_add(w);
    ws_slot_add(w);
    ws_slot_add(w);
    ws_to_store(&g_set_a, doc);

    ok(L"slot 2 exists", store_get(doc, "slot:ws:2", "has_override", NULL) != NULL);

    ws_slot_remove(w, 2);
    ws_to_store(&g_set_a, doc);

    ok(L"slot 2 is gone", store_get(doc, "slot:ws:2", "has_override", NULL) == NULL);
    ok(L"slot 1 is still there",
       store_get(doc, "slot:ws:1", "has_override", NULL) != NULL);

    /* And a reload must agree. */
    ws_clear(&g_set_b);
    ws_from_store(&g_set_b, doc);
    ok_int(L"reload sees two slots", g_set_b.items[0].slot_count, 2);

    store_destroy(doc);
}

static void test_hostile_section_names(void)
{
    T_SEC(L"sections a hand-edited file might contain");

    StoreDocument *doc = store_create();
    if (doc == NULL) return;

    /* Write a good workspace with one slot... */
    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "ws", "WS");
    WorkspaceSlot *s = ws_slot_add(w);
    strncpy_s(s->preset_id, sizeof(s->preset_id), "keepme", _TRUNCATE);
    ws_to_store(&g_set_a, doc);

    /* ...then add the things a user could type by accident. */
    store_set(doc, "slot:ws:99", "preset", "faraway");   /* past the maximum */
    store_set(doc, "slot:ws:1a", "preset", "notanumber");
    store_set(doc, "slot:ws:",   "preset", "nonumber");
    store_set(doc, "slot:ghost:0", "preset", "noworkspace");
    store_set(doc, "slot:", "preset", "noid");

    ws_clear(&g_set_b);
    ws_from_store(&g_set_b, doc);

    ok_int(L"one workspace loaded", g_set_b.count, 1);
    if (g_set_b.count == 1) {
        ok_int(L"still exactly one slot", g_set_b.items[0].slot_count, 1);
        ok_str(L"slot 0 kept its preset", g_set_b.items[0].slots[0].preset_id, "keepme");
        ok_str(L"slot 1 was NOT invented",
               g_set_b.items[0].slots[1].preset_id, "");
    }

    /*
     * Saving now cleans up the sections that ARE slot sections but no longer
     * have a slot behind them. "slot:ghost:0" IS removed - its workspace is
     * gone. "slot:ws:1a" is NOT: it does not parse as a slot section at all,
     * and a section this module cannot read might belong to a future format or
     * to something else entirely. Deleting what you do not understand is how a
     * tool eats the user's file.
     */
    ws_to_store(&g_set_b, doc);
    ok(L"the out-of-range section was cleaned up",
       store_get(doc, "slot:ws:99", "preset", NULL) == NULL);
    ok(L"the non-numeric section was left alone",
       store_get(doc, "slot:ws:1a", "preset", NULL) != NULL);
    ok(L"the unreadable section was left alone",
       store_get(doc, "slot:", "preset", NULL) != NULL);

    store_destroy(doc);
}

static void test_count_derived_without_slot_count_key(void)
{
    T_SEC(L"slot_count missing from the file");

    StoreDocument *doc = store_create();
    if (doc == NULL) return;

    store_set(doc, "workspace:hand", "name", "Hand written");
    store_set(doc, "slot:hand:0", "preset", "a");
    store_set(doc, "slot:hand:1", "preset", "b");

    ws_clear(&g_set_b);
    ws_from_store(&g_set_b, doc);

    ok_int(L"one workspace", g_set_b.count, 1);
    if (g_set_b.count == 1) {
        ok_int(L"two slots derived from the sections", g_set_b.items[0].slot_count, 2);
        ok_str(L"slot 0", g_set_b.items[0].slots[0].preset_id, "a");
        ok_str(L"slot 1", g_set_b.items[0].slots[1].preset_id, "b");
    }

    store_destroy(doc);
}

static void test_clear_does_not_touch_the_slots(void)
{
    T_SEC(L"ws_clear only resets the count");

    /*
     * This is a deliberate behaviour, not an oversight: zeroing 1.4 MB on
     * every load would hand the whole amount to the working set. The contract
     * is that ws_clear leaves the array alone and every entry point that hands
     * out a Workspace zeroes it first - which the round-trip test above
     * exercises.
     */
    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "a", "A");
    WorkspaceSlot *s = ws_slot_add(w);
    wcsncpy_s(s->app_path, WSS_APP_PATH_MAX, L"C:\\leftover.exe", _TRUNCATE);

    ws_clear(&g_set_a);
    ok_int(L"count is zero", g_set_a.count, 0);

    /* The stale bytes are still there - and that is fine, because the next
     * workspace handed out is zeroed before use. */
    Workspace *w2 = ws_add(&g_set_a, "b", "B");
    ok(L"a new workspace is handed out zeroed", w2 != NULL && w2->slot_count == 0);
    if (w2 != NULL) {
        ok_wstr(L"no stale path leaks into the new workspace",
                w2->slots[0].app_path, L"");
    }
}

static void test_slot_effective(void)
{
    T_SEC(L"slot + preset -> one effective rule");

    Rule preset;
    ZeroMemory(&preset, sizeof(preset));
    wcsncpy_s(preset.app.exe_name, WAC_EXE_MAX, L"notepad.exe", _TRUNCATE);
    wcsncpy_s(preset.app.exe_path, WAC_EXE_MAX, L"C:\\Windows\\notepad.exe", _TRUNCATE);
    preset.zone.x = 0.0f; preset.zone.y = 0.0f;
    preset.zone.w = 0.5f; preset.zone.h = 1.0f;
    preset.zone.min_w = 1; preset.zone.min_h = 1;
    strncpy_s(preset.id, RULE_ID_MAX, "notepad", _TRUNCATE);
    preset.enabled = 1;

    WorkspaceSlot slot;
    ws_slot_defaults(&slot);
    strncpy_s(slot.preset_id, WSS_PRESET_ID_MAX, "notepad", _TRUNCATE);

    Rule out;

    ok(L"resolves", ws_slot_effective(&slot, &preset, &out));
    ok_wstr(L"executable name comes from the preset", out.app.exe_name, L"notepad.exe");
    ok_wstr(L"executable path comes from the preset", out.app.exe_path,
            L"C:\\Windows\\notepad.exe");
    ok(L"zone comes from the preset", out.zone.w > 0.49f && out.zone.w < 0.51f);
    ok(L"the slot's own existence enables it", out.enabled != 0);

    /* With the override flag set, the slot's zone wins... */
    slot.has_override = TRUE;
    slot.zone.x = 0.5f; slot.zone.y = 0.0f;
    slot.zone.w = 0.5f; slot.zone.h = 1.0f;
    slot.zone.min_w = 1; slot.zone.min_h = 1;

    ok(L"resolves with an override", ws_slot_effective(&slot, &preset, &out));
    ok(L"the override zone wins", out.zone.x > 0.49f);
    ok_wstr(L"...and the preset still supplies the program", out.app.exe_name,
            L"notepad.exe");

    /* ...and with it clear, the preset's zone is untouched even if the slot
     * happens to hold numbers. That is the whole point of the flag. */
    slot.has_override = FALSE;
    ok(L"resolves without an override", ws_slot_effective(&slot, &preset, &out));
    ok(L"the slot's numbers are ignored", out.zone.x < 0.01f);

    /* A slot that names its own executable: the FILE NAME has to follow the
     * path, or the window search would look for a process called "". */
    wcsncpy_s(slot.app_path, WSS_APP_PATH_MAX, L"D:\\Tools\\编辑器.exe", _TRUNCATE);
    ok(L"resolves with its own path", ws_slot_effective(&slot, &preset, &out));
    ok_wstr(L"the path replaces the preset's", out.app.exe_path, L"D:\\Tools\\编辑器.exe");
    ok_wstr(L"and the file name is derived from it", out.app.exe_name, L"编辑器.exe");

    /* A standalone slot: no preset at all. */
    WorkspaceSlot lone;
    ws_slot_defaults(&lone);
    ok(L"a slot with neither a path nor a preset is refused",
       !ws_slot_effective(&lone, NULL, &out));

    wcsncpy_s(lone.app_path, WSS_APP_PATH_MAX, L"C:\\x\\y.exe", _TRUNCATE);
    ok(L"a standalone slot with a path resolves", ws_slot_effective(&lone, NULL, &out));
    ok_wstr(L"with its own file name", out.app.exe_name, L"y.exe");
    ok(L"and a usable default size", out.zone.w > 0.0f && out.zone.h > 0.0f);

    ok(L"NULL slot refused", !ws_slot_effective(NULL, &preset, &out));
    ok(L"NULL output refused", !ws_slot_effective(&slot, &preset, NULL));
}

static void test_monitor_binding(void)
{
    T_SEC(L"monitor binding (stage 7)");

    /*
     * A fresh slot is bound to nothing. That is what every configuration
     * written before this feature means, and it has to keep meaning "apply it
     * wherever the window is" - otherwise upgrading would orphan every
     * existing workspace.
     */
    WorkspaceSlot fresh;
    ws_slot_defaults(&fresh);
    ok(L"a new slot is not bound", !fresh.mon_set);

    ws_clear(&g_set_a);
    Workspace *w = ws_add(&g_set_a, "coding", "编码工作区");
    if (w == NULL) { ok(L"workspace created", 0); return; }

    /* Slot 0 is bound the way a slot recorded on the 150% laptop panel would
     * be; slot 1 deliberately stays unbound. */
    WorkspaceSlot *s0 = ws_slot_add(w);
    strncpy_s(s0->preset_id, sizeof(s0->preset_id), "notepad", _TRUNCATE);
    s0->mon_set = TRUE;
    wcsncpy_s(s0->mon_device,   WAC_MON_NAME_MAX,     L"\\\\.\\DISPLAY1", _TRUNCATE);
    wcsncpy_s(s0->mon_friendly, WAC_MON_FRIENDLY_MAX, L"B160QAN02.7",     _TRUNCATE);
    s0->mon_w = 2560; s0->mon_h = 1600;
    s0->mon_x = 2560; s0->mon_y = 0;

    WorkspaceSlot *s1 = ws_slot_add(w);
    strncpy_s(s1->preset_id, sizeof(s1->preset_id), "editor", _TRUNCATE);

    StoreDocument *doc = store_create();
    ok(L"document created", doc != NULL);
    if (doc == NULL) return;

    ws_to_store(&g_set_a, doc);

    /* An unbound slot must not grow monitor keys in the file. A configuration
     * that never used the feature keeps exactly the shape it had before. */
    ok_str(L"unbound slot writes no mon_device",
           store_get(doc, "slot:coding:1", "mon_device", "(absent)"), "(absent)");
    ok_str(L"bound slot writes its device name",
           store_get(doc, "slot:coding:0", "mon_device", "(absent)"), "\\\\.\\DISPLAY1");

    ws_clear(&g_set_b);
    ws_from_store(&g_set_b, doc);
    store_destroy(doc);

    ok_int(L"one workspace came back", g_set_b.count, 1);
    if (g_set_b.count != 1) return;

    const Workspace *r = &g_set_b.items[0];
    ok_int(L"two slots came back", r->slot_count, 2);
    if (r->slot_count != 2) return;

    ok(L"the bound slot came back bound", r->slots[0].mon_set);
    ok_wstr(L"device name survived", r->slots[0].mon_device, L"\\\\.\\DISPLAY1");
    ok_wstr(L"EDID name survived", r->slots[0].mon_friendly, L"B160QAN02.7");
    ok_int(L"resolution width survived", r->slots[0].mon_w, 2560);
    ok_int(L"resolution height survived", r->slots[0].mon_h, 1600);
    ok_int(L"origin x survived", r->slots[0].mon_x, 2560);
    ok_int(L"origin y survived", r->slots[0].mon_y, 0);

    ok(L"the unbound slot came back unbound", !r->slots[1].mon_set);
    ok_wstr(L"and with an empty device name", r->slots[1].mon_device, L"");

    /*
     * Geometry without a name is NOT a binding. A hand-edited or truncated
     * file could carry the numbers, but geometry alone cannot say WHICH
     * screen this was - and a wrong binding would move a window to the wrong
     * panel, which is worse than no binding at all.
     */
    {
        StoreDocument *d2 = store_create();
        ok(L"second document created", d2 != NULL);
        if (d2 != NULL) {
            store_set(d2, "workspace:x", "name", "X");
            store_set(d2, "workspace:x", "slot_count", "1");
            store_set(d2, "slot:x:0", "preset", "notepad");
            store_set(d2, "slot:x:0", "mon_w", "1920");
            store_set(d2, "slot:x:0", "mon_h", "1080");

            WorkspaceSet *tmp = &g_set_b;
            ws_clear(tmp);
            ws_from_store(tmp, d2);
            store_destroy(d2);

            ok_int(L"a geometry-only section still loads", tmp->count, 1);
            if (tmp->count == 1 && tmp->items[0].slot_count == 1) {
                ok(L"geometry without a name is not a binding",
                   !tmp->items[0].slots[0].mon_set);
            }
        }
    }
}

static MonitorFingerprint mkmon(const wchar_t *dev, const wchar_t *friendly,
                                int w, int h, int vx, int vy)
{
    MonitorFingerprint m;
    ZeroMemory(&m, sizeof(m));
    wcsncpy_s(m.device_name,   WAC_MON_NAME_MAX,     dev,      _TRUNCATE);
    wcsncpy_s(m.friendly_name, WAC_MON_FRIENDLY_MAX, friendly, _TRUNCATE);
    m.width = w; m.height = h; m.virtual_x = vx; m.virtual_y = vy;
    return m;
}

static void test_monitor_evaluation(void)
{
    T_SEC(L"monitor binding: what it means right now");

    /* ---- what bind() records, and what it refuses to record ---- */
    WorkspaceSlot s;
    ws_slot_defaults(&s);

    const MonitorFingerprint laptop = mkmon(L"\\\\.\\DISPLAY1", L"B160QAN02.7",
                                            2560, 1600, 2560, 0);
    ws_slot_bind_monitor(&s, &laptop);
    ok(L"a named fingerprint binds the slot", s.mon_set);
    ok_wstr(L"device name recorded", s.mon_device, L"\\\\.\\DISPLAY1");
    ok_wstr(L"EDID name recorded", s.mon_friendly, L"B160QAN02.7");
    ok_int(L"resolution recorded", s.mon_w, 2560);
    ok_int(L"origin recorded", s.mon_x, 2560);

    WorkspaceSlot anon;
    ws_slot_defaults(&anon);
    const MonitorFingerprint nameless = mkmon(L"", L"", 1920, 1080, 0, 0);
    ws_slot_bind_monitor(&anon, &nameless);
    ok(L"a fingerprint with no name does NOT bind", !anon.mon_set);

    /* ---- evaluating against a live set ---- */
    MonitorFingerprint cur[WAC_MON_MAX];
    cur[0] = mkmon(L"\\\\.\\DISPLAY1", L"B160QAN02.7", 2560, 1600, 2560, 0);
    cur[1] = mkmon(L"\\\\.\\DISPLAY6", L"H27T22X",     2560, 1440, 0,    0);
    int idx = -9;

    ok_int(L"the bound slot matches its own monitor",
           (int)ws_slot_evaluate_monitor(&s, cur, 2, &idx), (int)SLOT_READY);
    ok_int(L"on the right index", idx, 0);

    /* The device name was renumbered (what disabling and re-enabling the
     * adapter does - measured DISPLAY6 -> DISPLAY10). The EDID name has to
     * carry the match, which is exactly what the stage-3 fix made possible. */
    MonitorFingerprint renamed[WAC_MON_MAX];
    renamed[0] = mkmon(L"\\\\.\\DISPLAY10", L"B160QAN02.7", 2560, 1600, 2560, 0);
    ok_int(L"an EDID match survives a renumbered device name",
           (int)ws_slot_evaluate_monitor(&s, renamed, 1, &idx), (int)SLOT_READY);
    ok_int(L"and still finds the panel", idx, 0);

    /* The recorded monitor is gone. */
    MonitorFingerprint other_only[WAC_MON_MAX];
    other_only[0] = mkmon(L"\\\\.\\DISPLAY6", L"H27T22X", 2560, 1440, 0, 0);
    ok_int(L"a missing monitor orphans the slot",
           (int)ws_slot_evaluate_monitor(&s, other_only, 1, &idx), (int)SLOT_ORPHANED);
    ok_int(L"and reports no index", idx, -1);

    ok_int(L"an empty monitor set orphans it too",
           (int)ws_slot_evaluate_monitor(&s, cur, 0, &idx), (int)SLOT_ORPHANED);

    /* Unbound slots never orphan. This is what keeps every configuration
     * written before stage 7 working. */
    ok_int(L"an unbound slot is always ready",
           (int)ws_slot_evaluate_monitor(&anon, cur, 2, &idx), (int)SLOT_READY);
    ok_int(L"even with no monitors at all",
           (int)ws_slot_evaluate_monitor(&anon, cur, 0, &idx), (int)SLOT_READY);

    /* ---- geometry-only agreement is evidence, not proof ---- */
    WorkspaceSlot geo;
    ws_slot_defaults(&geo);
    geo.mon_set = TRUE;              /* as a hand-edited file could leave it */
    geo.mon_w = 2560; geo.mon_h = 1600; geo.mon_x = 2560; geo.mon_y = 0;
    ok_int(L"geometry alone is REBOUND, not READY",
           (int)ws_slot_evaluate_monitor(&geo, cur, 2, &idx), (int)SLOT_REBOUND);
    ok_int(L"and it does find the panel", idx, 0);

    /* ---- a much narrower panel degrades the slot ---- */
    WorkspaceSlot wide;
    ws_slot_defaults(&wide);
    ws_slot_bind_monitor(&wide, &laptop);        /* recorded at 2560 wide */

    MonitorFingerprint narrow[WAC_MON_MAX];
    narrow[0] = mkmon(L"\\\\.\\DISPLAY1", L"B160QAN02.7", 1920, 1200, 0, 0);
    ok_int(L"a much narrower panel degrades the slot",
           (int)ws_slot_evaluate_monitor(&wide, narrow, 1, &idx), (int)SLOT_DEGRADED);

    /* Inside the band it is still the same panel: the same threshold
     * rel_resolve_size() uses, so the two cannot disagree. */
    MonitorFingerprint slightly[WAC_MON_MAX];
    slightly[0] = mkmon(L"\\\\.\\DISPLAY1", L"B160QAN02.7", 2400, 1600, 0, 0);
    ok_int(L"6.7% narrower stays READY",
           (int)ws_slot_evaluate_monitor(&wide, slightly, 1, &idx), (int)SLOT_READY);
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t **argv)
{
    int no_pause = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--no-pause")) no_pause = 1;
    }

    t_diag_init(L"t18_workspace");
    t_log_mirror(1);
    t_console_init();

    T_TITLE(L"t18_workspace - workspace slots");
    T_KV(L"run_at", L"%ls", t_now_string());

    test_defaults();
    test_add_remove();
    test_round_trip();
    test_override_is_not_written_when_off();
    test_removed_workspace_takes_its_slots();
    test_shrinking_removes_trailing_slot_sections();
    test_hostile_section_names();
    test_count_derived_without_slot_count_key();
    test_clear_does_not_touch_the_slots();
    test_slot_effective();
    test_monitor_binding();
    test_monitor_evaluation();

    T_SEC(L"Result");
    T_KV(L"passed", L"%d", g_pass);
    T_KV(L"failed", L"%d", g_fail);

    if (g_fail == 0) {
        T_KV(L"VERDICT", L"PASS - all %d assertions hold", g_pass);
    } else {
        T_KV(L"VERDICT", L"FAIL - %d assertion(s) broken", g_fail);
    }

    if (!no_pause) t_pause();

    t_log_close();
    return (g_fail == 0) ? 0 : 1;
}
