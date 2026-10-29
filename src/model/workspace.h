/*
 * workspace.h - the in-memory workspace list, and its bridge to the store.
 *
 * SCOPE
 *
 *   Stage 2 introduced this type with an id, a display name and a slot count.
 *   Stage 5 fills in the slots themselves: what to launch, in which order, and
 *   where the window should end up.
 *
 * WHY A SEPARATE MODEL
 *
 *   The store deals in sections called "workspace:<id>" and "slot:<id>:<n>".
 *   That naming is a persistence detail. Keeping it out of the UI means the
 *   window code never has to build a section name, and a future format
 *   change touches only the two functions at the bottom of this file.
 *
 * SIZING - READ BEFORE DECLARING ONE OF THESE
 *
 *   A Workspace is now about 22 KB (16 slots), so a WorkspaceSet is about
 *   1.4 MB. That is too much for a stack frame: TRAPS.md #11 is
 *   exactly this mistake, and the result is an instant 0xC00000FD with no
 *   output at all. Declare these as `static`, as main.c does, or heap-allocate
 *   them.
 *
 *   ws_clear() deliberately does NOT memset the array - it resets the count
 *   and leaves the slots alone. Zeroing 1.4 MB on every load would touch every
 *   page of it and hand the whole amount to the working set, which is the
 *   mistake recorded as trap 17. Every entry point zeroes the one element it
 *   is about to hand out, so nothing ever reads a stale slot.
 */

#ifndef WAC_WORKSPACE_H
#define WAC_WORKSPACE_H

#include <windows.h>

#include "proc_launch.h"   /* LaunchKind - how a slot's program is started */
#include "relcoord.h"
#include "rule.h"          /* Rule - what a slot refers to */
#include "monitor.h"       /* MonitorFingerprint - the monitor a slot was bound to */

#ifdef __cplusplus
extern "C" {
#endif

#define WS_ID_MAX     64
#define WS_NAME_MAX   128
#define WS_MAX        64    /* workspaces a user may define */

/* ---- slots -------------------------------------------------------- */

#define WSS_SLOT_MAX        16    /* applications in one workspace */
#define WSS_PRESET_ID_MAX   64    /* the [rule:*] id this slot refers to */
#define WSS_APP_PATH_MAX    260   /* MAX_PATH, wide */
#define WSS_ARGS_MAX        256   /* launch arguments, wide */
#define WSS_WAIT_DEFAULT_MS 15000 /* how long to wait for the window */

/*
 * Whether the slot's target can be placed.
 *
 * The last three are about the MONITOR a slot was recorded on, which is why
 * they live beside the slot rather than in the result of one launch: a
 * workspace can be applied while one of its monitors is unplugged, and the
 * answer has to survive that.
 */
typedef enum {
    SLOT_READY = 0,   /* the monitor is there; this slot can be applied */
    SLOT_ORPHANED,    /* the recorded monitor is gone - keep the config, skip */
    SLOT_REBOUND,     /* matched a different monitor, with fingerprint evidence */
    SLOT_DEGRADED     /* resolution changed a lot; sizes may not be ideal */
} SlotState;

/*
 * How the program is started lives in core/proc_launch.h, because that is the
 * module that acts on it. A slot only records the choice.
 */

/*
 * One application in a workspace.
 *
 * REFERENCE SEMANTICS
 *
 *   A slot does not own its coordinates. It refers to a window preset
 *   ([rule:<id>]) and only carries the fields where THIS workspace differs
 *   from that preset. That is why `preset_id` comes first: the preset is the
 *   answer to "what program, where", and the rest of the struct is the list of
 *   exceptions.
 *
 *   A slot with an empty preset_id is standalone and must carry its own
 *   launch path; the executor reports it as NOT_FOUND rather than guessing.
 */
typedef struct {
    /* ---- the reference ---- */
    char preset_id[WSS_PRESET_ID_MAX];  /* empty = standalone slot */
    BOOL has_override;                  /* 0 = everything comes from the preset */

    /* ---- launching ---- */
    wchar_t    app_path[WSS_APP_PATH_MAX]; /* empty = use the preset's path */
    wchar_t    launch_args[WSS_ARGS_MAX];  /* appended to the command line */
    LaunchKind launch_kind;
    int        wait_timeout_ms;            /* 0 = WSS_WAIT_DEFAULT_MS */
    int        order;                      /* 0 = parallel, >0 = sequential */

    /* ---- zone override (only read when has_override is set) ---- */
    RelRect zone;

    /* ---- monitor binding (stage 7) ------------------------------------
     *
     * Which MONITOR this slot was recorded on. It lives beside the slot, not
     * in the result of one launch, because a workspace can be applied while
     * one of its screens is unplugged and the answer has to survive that.
     *
     * ALL THREE LEVELS of the matching cascade are stored, because the
     * measured behaviour (six operations each run on real hardware) is that
     * different operations break different levels:
     *
     *   mon_device    "\\.\DISPLAY6" - survives a physical unplug/replug, a
     *                 primary change, a resolution change and sleep/wake, but
     *                 NOT the display adapter being disabled and re-enabled:
     *                 that hands out a NEW number (measured DISPLAY6 ->
     *                 DISPLAY10)
     *   mon_friendly  the EDID name - survives everything measured so far,
     *                 and is the ONLY level left once an adapter has been
     *                 disabled. This is why the stage-3 fix to the EDID name
     *                 lookup is a prerequisite for this feature
     *   mon_w/h/x/y   the geometry - survives the adapter case, but not the
     *                 three operations that re-lay-out the desktop (unplugging
     *                 the primary, changing the primary, changing resolution)
     *
     * mon_set is FALSE for a slot that was never bound - which is every slot
     * written by an older build. That means "apply it wherever the window
     * is", exactly the behaviour that existed before stage 7, so an existing
     * configuration keeps working unchanged.
     */
    BOOL      mon_set;
    wchar_t   mon_device[WAC_MON_NAME_MAX];        /* "\\.\DISPLAYn" at record time */
    wchar_t   mon_friendly[WAC_MON_FRIENDLY_MAX];  /* EDID name at record time */
    int       mon_w, mon_h;                        /* resolution at record time */
    int       mon_x, mon_y;                        /* virtual origin at record time */

    /* ---- runtime only; never persisted ---- */
    SlotState state;
    DWORD     pid;
    HWND      hwnd;
} WorkspaceSlot;

typedef struct {
    char id[WS_ID_MAX];       /* stable identifier, used in section names */
    char name[WS_NAME_MAX];   /* what the user sees (UTF-8) */

    int slot_count;           /* how many of `slots` are in use */
    WorkspaceSlot slots[WSS_SLOT_MAX];
} Workspace;

typedef struct {
    Workspace items[WS_MAX];
    int       count;
} WorkspaceSet;

/* ---- basic operations -------------------------------------------- */

void ws_clear(WorkspaceSet *set);

/* Find by id, or by name when id does not match. NULL when absent. */
Workspace *ws_find(WorkspaceSet *set, const char *id);
const Workspace *ws_find_const(const WorkspaceSet *set, const char *id);

/*
 * Add a workspace. The id is generated from `name` if `id` is NULL or
 * empty, made unique within the set. Returns NULL when the set is full or
 * the name is empty.
 */
Workspace *ws_add(WorkspaceSet *set, const char *id, const char *name);

/* Remove by id. Returns TRUE when something was removed. */
BOOL ws_remove(WorkspaceSet *set, const char *id);

/*
 * Turn a display name into a usable id: lowercase ASCII, digits, '-' and
 * '_' only, truncated. Collisions get a numeric suffix.
 *
 * The result goes into section names in the config file, so it must stay
 * free of characters that would break the INI syntax or surprise a user
 * editing the file by hand.
 */
void ws_make_id(const WorkspaceSet *set, const char *name,
                char *out, size_t out_size);

/* Rename, keeping the id (and therefore the stored slots) intact. */
BOOL ws_rename(WorkspaceSet *set, const char *id, const char *new_name);

/* ---- slots -------------------------------------------------------- */

/*
 * Append an empty slot. Returns NULL when the workspace has WSS_SLOT_MAX
 * slots already. The caller fills in what it needs; everything starts at its
 * "inherit" value (empty preset, no override, parallel order).
 */
WorkspaceSlot *ws_slot_add(Workspace *w);

/*
 * Remove slot `index`, shifting the rest down.
 *
 * Slots are addressed by POSITION and stored as [slot:<id>:<n>] with n being
 * that position, so removing one renumbers the ones after it. ws_to_store()
 * rewrites every slot section of the workspace, which is what makes the
 * renumbering consistent rather than leaving a hole at n.
 */
BOOL ws_slot_remove(Workspace *w, int index);

/* The value an unset field means. Used by the setters and by the loader, so
 * "absent" and "explicitly default" cannot drift apart. */
void ws_slot_defaults(WorkspaceSlot *s);

/*
 * Combine a slot with the window preset it refers to into ONE rule that says
 * what to start and where it goes.
 *
 * PURE: no window calls, no file access, no globals. That is what lets the
 * reference semantics be tested without launching anything - the same reason
 * rule_target_rect and monitor_match are pure.
 *
 *     preset != NULL   start from the preset. A slot with has_override
 *                      replaces the zone; a slot with its own app_path
 *                      replaces the executable.
 *     preset == NULL   the slot stands alone and must carry its own path.
 *
 * Returns FALSE when the result would not identify a program at all (no file
 * name and no path). The executor reports that as SKIPPED rather than trying
 * to start nothing and calling the failure a timeout.
 */
BOOL ws_slot_effective(const WorkspaceSlot *slot, const Rule *preset, Rule *out);

/* ---- monitor binding (stage 7) ------------------------------------ */

/*
 * The state's name, for logs and the CLI. English and never translated: the
 * log is grepped, not read aloud. Never NULL.
 */
const wchar_t *ws_slot_state_name(SlotState st);

/*
 * Record the monitor this slot belongs to.
 *
 * Copies all three levels of the fingerprint - see the struct's comment for
 * why one is not enough. A fingerprint carrying neither a device name nor an
 * EDID name cannot identify a monitor later, so such a slot stays UNBOUND
 * rather than becoming permanently ORPHANED.
 */
void ws_slot_bind_monitor(WorkspaceSlot *s, const MonitorFingerprint *fp);

/*
 * Decide what the slot's monitor binding means right now.
 *
 * PURE: the current monitor set is a parameter, no enumeration, no OS calls -
 * the same shape as monitor_match(), and for the same reason. This is the
 * logic that has to be right when a monitor is unplugged, and that situation
 * cannot be arranged on demand at a desk.
 *
 * Returns the SlotState to record, and stores the index within `cur` into
 * *out_index whenever something matched:
 *
 *   SLOT_READY     not bound, or matched by device name / EDID name, and the
 *                  panel is about the size it was
 *   SLOT_DEGRADED  matched, but the panel is more than REL_JITTER_FRACTION
 *                  narrower than the one it was recorded on - a zone no
 *                  longer produces the sizes the user saw
 *   SLOT_REBOUND   matched on geometry alone: evidence, not proof
 *   SLOT_ORPHANED  nothing matched - the recorded monitor is gone
 *
 * An UNBOUND slot is always SLOT_READY. That is the pre-stage-7 behaviour and
 * it has to stay that way, or every configuration written before this feature
 * existed would look orphaned.
 */
SlotState ws_slot_evaluate_monitor(const WorkspaceSlot *s,
                                   const MonitorFingerprint *cur, int count,
                                   int *out_index);

/* ---- persistence bridge ------------------------------------------ */

/*
 * Fill `set` from a loaded document.
 *
 * Section names are the contract:
 *     [workspace:<id>]   name=<display name>   [, slot_count=<n>]
 *     [slot:<id>:<n>]    preset=, order=, ...   (n is the position, 0-based)
 *
 * Sections that do not match the pattern are ignored, so an unrelated or
 * future section cannot break loading. When slot_count is absent it is
 * derived by counting [slot:<id>:*] sections, which keeps hand-written
 * files working.
 *
 * A slot section whose number is beyond WSS_SLOT_MAX is skipped rather than
 * wrapping around: a hand-edited file with [slot:x:99] must not overwrite
 * slot 3.
 */
void ws_from_store(WorkspaceSet *set, const void *store_doc);

/*
 * Write `set` into a document, leaving unrelated sections (meta, and any
 * future ones) untouched.
 *
 * Removes workspace sections that are no longer in the set, so deleting a
 * workspace actually deletes it rather than leaving an orphan behind, and
 * removes slot sections of a workspace that shrank.
 */
void ws_to_store(const WorkspaceSet *set, void *store_doc);

/* ---- defaults ----------------------------------------------------- */

/* The workspace created on first run, so the window is never empty. */
void ws_seed_defaults(WorkspaceSet *set);

#ifdef __cplusplus
}
#endif

#endif /* WAC_WORKSPACE_H */
