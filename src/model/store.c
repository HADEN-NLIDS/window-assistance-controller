/*
 * store.c - INI persistence with atomic writes, backups and a checksum.
 *
 * See store.h for why this module is deliberate about durability, and for
 * the SIZING WARNING: a fully populated document is ~26 MB, so it must
 * never live on the stack. That is why the type is opaque and everything
 * goes through store_create().
 *
 * Implementation notes worth keeping in mind when editing:
 *
 *  - The parser is line-oriented and never grows the document beyond its
 *    fixed arrays. A malformed or hostile file can therefore make parsing
 *    fail, but it cannot make the program allocate unbounded memory or
 *    overrun a buffer.
 *
 *  - Keys and values are stored as narrow (char) strings. The config file
 *    is ASCII by construction: keys are identifiers, values are numbers or
 *    narrow identifiers.
 *
 *  - The checksum covers section names, keys and values in a fixed order,
 *    so it is independent of blank lines, comments and file layout - which
 *    is what a hand-edited file needs.
 *
 *  - store_save() stamps the checksum into a COPY of the content it is
 *    about to write, so the caller never has to remember to.
 */

#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal layout (hidden from callers on purpose)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char key[STORE_KEY_MAX];
    char value[STORE_VALUE_MAX];
} StoreKV;

typedef struct {
    char    name[STORE_KEY_MAX];
    StoreKV items[STORE_MAX_KEYS];
    int     count;
} StoreSection;

/*
 * Sections are allocated on demand.
 *
 * A previous revision held them inline:
 *
 *     StoreSection sections[STORE_MAX_SECTIONS];
 *
 * which is 256 x (64 + 128 x (64 + 768)) = about 26 MB, and store_create()
 * touched all of it through calloc, so the working set of an idle program
 * was 59.8 MB - four times the 15 MB budget in the plan.
 *
 * A real configuration uses a handful of sections totalling a few KB, so
 * 99.9% of that memory was reserved for a worst case that does not occur.
 *
 * The array of pointers costs 256 x 8 = 2 KB, and each section is 104 KB
 * allocated only when the file actually contains one.
 */
struct StoreDocument {
    StoreSection *sections[STORE_MAX_SECTIONS];
    int           count;
};

StoreDocument *store_create(void)
{
    /* Only the pointer array is allocated here: a couple of KB, not 26 MB.
     * Sections appear as add_section() is called during parsing. */
    return (StoreDocument *)calloc(1, sizeof(StoreDocument));
}

void store_destroy(StoreDocument *doc)
{
    if (doc == NULL) return;

    for (int i = 0; i < doc->count; ++i) {
        free(doc->sections[i]);
    }
    free(doc);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void copy_z(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    if (src == NULL) { dst[0] = '\0'; return; }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s) {
        const char c = end[-1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') end--;
        else break;
    }
    *end = '\0';
    return s;
}

static int is_ignorable(const char *s)
{
    return s[0] == '\0' || s[0] == ';' || s[0] == '#';
}

/* ------------------------------------------------------------------ */
/* Checksum (FNV-1a, 32-bit)                                           */
/* ------------------------------------------------------------------ */

unsigned store_checksum_bytes(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned h = 2166136261u;                 /* FNV offset basis */
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned)p[i];
        h *= 16777619u;                       /* FNV prime */
    }
    return h;
}

static unsigned doc_checksum(const StoreDocument *doc)
{
    unsigned h = 2166136261u;

    for (int s = 0; s < doc->count; ++s) {
        const StoreSection *sec = doc->sections[s];
        if (sec == NULL) continue;

        /* Skip the meta section: it holds the checksum itself. */
        if (_stricmp(sec->name, "meta") == 0) continue;

        h ^= store_checksum_bytes(sec->name, strlen(sec->name));
        h *= 16777619u;
        h ^= (unsigned)'\x1F';
        h *= 16777619u;

        for (int k = 0; k < sec->count; ++k) {
            const StoreKV *kv = &sec->items[k];
            h ^= store_checksum_bytes(kv->key, strlen(kv->key));
            h *= 16777619u;
            h ^= (unsigned)'=';
            h *= 16777619u;
            h ^= store_checksum_bytes(kv->value, strlen(kv->value));
            h *= 16777619u;
            h ^= (unsigned)'\x1E';
            h *= 16777619u;
        }
    }
    return h;
}

void store_stamp_checksum(StoreDocument *doc)
{
    if (doc == NULL) return;

    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08X", doc_checksum(doc));
    store_set(doc, "meta", "checksum", buf);
}

/* ------------------------------------------------------------------ */
/* Document access                                                     */
/* ------------------------------------------------------------------ */

static StoreSection *find_section(StoreDocument *doc, const char *name)
{
    if (doc == NULL || name == NULL) return NULL;
    for (int i = 0; i < doc->count; ++i) {
        if (doc->sections[i] != NULL &&
            _stricmp(doc->sections[i]->name, name) == 0) {
            return doc->sections[i];
        }
    }
    return NULL;
}

static StoreSection *add_section(StoreDocument *doc, const char *name)
{
    if (doc == NULL || name == NULL) return NULL;

    StoreSection *existing = find_section(doc, name);
    if (existing) return existing;

    if (doc->count >= STORE_MAX_SECTIONS) return NULL;

    StoreSection *sec = (StoreSection *)calloc(1, sizeof(StoreSection));
    if (sec == NULL) return NULL;   /* out of memory: caller reports overflow */

    doc->sections[doc->count++] = sec;
    copy_z(sec->name, sizeof(sec->name), name);
    return sec;
}

static StoreKV *section_find_key(StoreSection *sec, const char *key)
{
    for (int i = 0; i < sec->count; ++i) {
        if (_stricmp(sec->items[i].key, key) == 0) return &sec->items[i];
    }
    return NULL;
}

/* ---- iteration API ------------------------------------------------- */

int store_section_count(const StoreDocument *doc)
{
    return doc ? doc->count : 0;
}

void *store_section_at(const StoreDocument *doc, int index)
{
    if (doc == NULL || index < 0 || index >= doc->count) return NULL;
    return (void *)doc->sections[index];
}

const char *store_section_name(const void *section)
{
    const StoreSection *sec = (const StoreSection *)section;
    return sec ? sec->name : NULL;
}

int store_section_key_count(const void *section)
{
    const StoreSection *sec = (const StoreSection *)section;
    return sec ? sec->count : 0;
}

const char *store_section_key_at(const void *section, int index)
{
    const StoreSection *sec = (const StoreSection *)section;
    if (sec == NULL || index < 0 || index >= sec->count) return NULL;
    return sec->items[index].key;
}

const char *store_section_value_at(const void *section, int index)
{
    const StoreSection *sec = (const StoreSection *)section;
    if (sec == NULL || index < 0 || index >= sec->count) return NULL;
    return sec->items[index].value;
}

BOOL store_has_section(const StoreDocument *doc, const char *name)
{
    return find_section((StoreDocument *)doc, name) != NULL;
}

void store_remove_section(StoreDocument *doc, const char *name)
{
    if (doc == NULL || name == NULL) return;

    for (int i = 0; i < doc->count; ++i) {
        StoreSection *sec = doc->sections[i];
        if (sec == NULL) continue;
        if (_stricmp(sec->name, name) != 0) continue;

        /* Shift the tail down, then free the removed section.
         *
         * The free is not optional: sections are heap blocks now, so moving
         * the pointers without releasing the displaced one would leak about
         * 100 KB every time a workspace is deleted. */
        for (int j = i; j + 1 < doc->count; ++j) {
            doc->sections[j] = doc->sections[j + 1];
        }
        doc->sections[doc->count - 1] = NULL;
        doc->count--;

        free(sec);
        return;
    }
}

/* ---- get / set ----------------------------------------------------- */

const char *store_get(const StoreDocument *doc, const char *section,
                      const char *key, const char *fallback)
{
    if (doc == NULL || section == NULL || key == NULL) return fallback;

    StoreSection *sec = find_section((StoreDocument *)doc, section);
    if (sec == NULL) return fallback;

    StoreKV *kv = section_find_key(sec, key);
    if (kv == NULL) return fallback;
    return kv->value;
}

void store_set(StoreDocument *doc, const char *section, const char *key, const char *value)
{
    if (doc == NULL || section == NULL || key == NULL) return;

    StoreSection *sec = add_section(doc, section);
    if (sec == NULL) return;   /* document full: drop the write, never crash */

    StoreKV *kv = section_find_key(sec, key);
    if (kv == NULL) {
        if (sec->count >= STORE_MAX_KEYS) return;
        kv = &sec->items[sec->count++];
        memset(kv, 0, sizeof(*kv));
        copy_z(kv->key, sizeof(kv->key), key);
    }
    copy_z(kv->value, sizeof(kv->value), value ? value : "");
}

/* ------------------------------------------------------------------ */
/* Typed readers                                                       */
/* ------------------------------------------------------------------ */

int store_get_int(const StoreDocument *doc, const char *section,
                  const char *key, int fallback)
{
    const char *v = store_get(doc, section, key, NULL);
    if (v == NULL || *v == '\0') return fallback;

    char *end = NULL;
    long n = strtol(v, &end, 10);

    /* Reject trailing garbage: "12abc" is a malformed value, not 12. */
    if (end == v) return fallback;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return fallback;

    if (n > 2147483647L) return 2147483647;
    if (n < -2147483647L - 1L) return -2147483647 - 1;
    return (int)n;
}

float store_get_float(const StoreDocument *doc, const char *section,
                      const char *key, float fallback)
{
    const char *v = store_get(doc, section, key, NULL);
    if (v == NULL || *v == '\0') return fallback;

    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return fallback;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return fallback;

    /* NaN or infinity in the file must not propagate into geometry. */
    if (d != d) return fallback;
    if (d > 1.0e9 || d < -1.0e9) return fallback;

    return (float)d;
}

void store_format_float(char *out, size_t out_size, float v)
{
    if (out == NULL || out_size == 0) return;

    /*
     * Deliberately not "%.4f": in a locale where the decimal separator is a
     * comma, printf would emit "0,5000" and the file would no longer parse.
     * Build the string from an integer instead.
     *
     * THE SIGN NEEDS HANDLING SEPARATELY. Measured: -0.25 scaled to
     * scaled = -2500, whole = 0, frac = -2500. Formatting those as
     * "%d.%04d" with frac negated produced "0.2500" - the minus sign lived
     * only in frac, and negating it discarded it. Any negative coordinate
     * smaller than 1 in magnitude would have been stored as positive, which
     * matters because a monitor left of the primary has negative origins.
     *
     * So emit the sign from `scaled` and format the magnitude.
     */
    const int scaled = (int)(v * 10000.0f + (v >= 0.0f ? 0.5f : -0.5f));

    const int mag   = (scaled < 0) ? -scaled : scaled;
    const int whole = mag / 10000;
    const int frac  = mag % 10000;

    _snprintf_s(out, out_size, _TRUNCATE, "%s%d.%04d",
                (scaled < 0) ? "-" : "", whole, frac);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *cur;
    const char *end;
} Cursor;

static BOOL next_line(Cursor *c, char *buf, size_t cap)
{
    if (c->cur >= c->end) return FALSE;

    size_t n = 0;
    while (c->cur < c->end && *c->cur != '\n') {
        if (n + 1 < cap) buf[n++] = *c->cur;
        c->cur++;
    }
    if (c->cur < c->end && *c->cur == '\n') c->cur++;
    buf[n] = '\0';
    return TRUE;
}

/*
 * Parse a whole buffer into an ALREADY ALLOCATED document.
 *
 * Returns FALSE only when the document overflows its fixed arrays, which
 * means the file is not one this program wrote. Individual malformed lines
 * are skipped: a user's stray edit should cost them that one setting, not
 * the entire file.
 */
static BOOL parse_into(StoreDocument *doc, const char *data, size_t len)
{
    /*
     * Release any sections already present before clearing the count.
     *
     * This replaced `memset(doc, 0, sizeof(*doc))`, which used to be correct
     * when sections were stored inline. Now that they are heap blocks, a
     * memset would drop every pointer and leak them - and the document
     * arrives already empty on the normal path, so this loop usually does
     * nothing at all.
     */
    for (int i = 0; i < doc->count; ++i) {
        free(doc->sections[i]);
        doc->sections[i] = NULL;
    }
    doc->count = 0;

    Cursor c = { data, data + len };
    char line[STORE_LINE_MAX];
    StoreSection *cur = NULL;
    BOOL overflow = FALSE;

    while (next_line(&c, line, sizeof(line))) {
        char *s = trim(line);
        if (is_ignorable(s)) continue;

        if (s[0] == '[') {
            char *close = strchr(s, ']');
            if (close == NULL) continue;      /* malformed header: skip */
            *close = '\0';
            char *name = trim(s + 1);
            if (*name == '\0') continue;

            cur = add_section(doc, name);
            if (cur == NULL) overflow = TRUE;
            continue;
        }

        char *eq = strchr(s, '=');
        if (eq == NULL) continue;             /* not a key=value line */

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (*key == '\0') continue;

        /* A key before any [section] is not addressable; skip it. */
        if (cur == NULL) continue;

        if (cur->count >= STORE_MAX_KEYS) { overflow = TRUE; continue; }
        StoreKV *kv = &cur->items[cur->count++];
        memset(kv, 0, sizeof(*kv));
        copy_z(kv->key, sizeof(kv->key), key);
        copy_z(kv->value, sizeof(kv->value), val);
    }

    return !overflow;
}

/* ------------------------------------------------------------------ */
/* Reading files                                                       */
/* ------------------------------------------------------------------ */

static BOOL read_whole_file(const wchar_t *path, char **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len = 0;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)(8 * 1024 * 1024)) {
        CloseHandle(h);           /* empty or absurdly large: unreadable */
        return FALSE;
    }

    const size_t len = (size_t)size.QuadPart;
    char *buf = (char *)malloc(len + 1);
    if (buf == NULL) { CloseHandle(h); return FALSE; }

    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)len, &got, NULL) || got == 0) {
        free(buf);
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);

    buf[got] = '\0';
    *out_data = buf;
    *out_len = got;
    return TRUE;
}

/*
 * Verify the checksum recorded in the file.
 *    1  present and correct
 *    0  absent (older or hand-made) - acceptable
 *   -1  present but WRONG - the file is damaged
 */
static int verify_checksum(StoreDocument *doc)
{
    const char *recorded = store_get(doc, "meta", "checksum", NULL);
    if (recorded == NULL || *recorded == '\0') return 0;

    char *end = NULL;
    unsigned want = (unsigned)strtoul(recorded, &end, 0);
    if (end == recorded) return 0;           /* unparsable: ignore */

    return (doc_checksum(doc) == want) ? 1 : -1;
}

/*
 * The file could not be parsed: try the rolling backups, most recent first.
 *
 * THIS is what the backups are for. Until 2026-09-22 the loop lived under the
 * checksum-mismatch branch instead, which meant the case backups exist for -
 * a file truncated by another program - never got one, while a file that
 * merely carried a stale checksum was silently replaced by an older copy.
 *
 * A backup is judged by whether it PARSES. Its checksum is not consulted: a
 * backup this program wrote always has one, and refusing a readable backup
 * over a number would leave the user with nothing.
 */
static StoreLoadResult try_backups(const wchar_t *path, StoreDocument **out)
{
    for (int i = 0; i < STORE_BACKUP_COUNT; ++i) {
        wchar_t bpath[512];
        if (!store_backup_path(path, i, bpath, 512)) break;

        char  *bdata = NULL;
        size_t blen  = 0;
        if (!read_whole_file(bpath, &bdata, &blen)) continue;

        StoreDocument *bstage = store_create();
        if (bstage == NULL) { free(bdata); break; }

        const BOOL ok = parse_into(bstage, bdata, blen);
        free(bdata);

        if (ok) {
            *out = bstage;
            return STORE_RECOVERED_FROM_BACKUP;
        }
        store_destroy(bstage);
    }
    return STORE_CORRUPT_NO_RECOVERY;
}

StoreLoadResult store_load(const wchar_t *path, StoreDocument **out)
{
    if (path == NULL || out == NULL) return STORE_CORRUPT_NO_RECOVERY;
    *out = NULL;

    char  *data = NULL;
    size_t len  = 0;

    if (!read_whole_file(path, &data, &len)) {
        /* Distinguish "not there yet" from "there but unreadable". */
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return STORE_MISSING;
        return STORE_CORRUPT_NO_RECOVERY;   /* locked, empty, or oversized */
    }

    StoreDocument *doc = store_create();
    if (doc == NULL) { free(data); return STORE_OUT_OF_MEMORY; }

    BOOL parsed = parse_into(doc, data, len);
    free(data);

    /*
     * Genuinely damaged: the bytes do not describe an INI document at all.
     * That is the one case the backups were kept for.
     */
    if (!parsed) {
        store_destroy(doc);
        return try_backups(path, out);
    }

    /*
     * Parsed - but it produced NO sections at all.
     *
     * MEASURED 2026-10-09, found by running the stage-7 acceptance list against
     * the product rather than the unit tests: a config file containing
     * "@@@ truncated @@@" was accepted, because the parser skips lines that are
     * neither a section header nor key=value (deliberately: an unrelated or
     * future section must not break loading). The document then held nothing,
     * so the program seeded its defaults and - on the next save - WROTE OVER
     * the file. The three rotating backups were never consulted, and the user
     * was never told: exactly the silent loss the whole module exists to
     * prevent (see the header, and trap 31 in docs/手册.md).
     *
     * Every file this program writes carries at least [meta] (version +
     * checksum), so "parsed, and empty" cannot be a file we wrote. It is junk,
     * or a file cut off before its first section header. Either way the
     * backups are the better answer, and with none the caller gets
     * CORRUPT_NO_RECOVERY and warns the user instead of quietly starting over.
     *
     * This does NOT narrow the "a file edited elsewhere is used" rule below:
     * that rule is about content we can read. Content we cannot recognize at
     * all is not somebody's edit.
     */
    if (store_section_count(doc) == 0) {
        store_destroy(doc);
        return try_backups(path, out);
    }

    int check = verify_checksum(doc);
    if (check >= 0) {
        *out = doc;
        return (check == 1) ? STORE_OK : STORE_OK_NO_CHECKSUM;
    }

    /*
     * The file PARSED but its checksum does not match, which means something
     * other than this program wrote it - a text editor, most likely.
     *
     * ACCEPTED. This used to fall through to the backup path, and the result
     * was that a user who added a section by hand saw it silently discarded:
     * the program loaded an older configuration instead and said only that the
     * file "was damaged". Measured 2026-09-22; see the note in store.h.
     *
     * The checksum exists to catch a half-written file. Atomic writing already
     * rules that out for anything this program writes, and a document that got
     * this far has demonstrably parsed - a truncated file does not.
     */
    *out = doc;
    return STORE_MODIFIED_EXTERNALLY;
}

StoreDocument *store_open_for_write(const wchar_t *path)
{
    StoreDocument *doc = NULL;

    if (path == NULL) return NULL;

    /*
     * Load first, THEN modify. Building a fresh document here is what deleted
     * every [rule:*] section on exit - see the note in store.h for the
     * measurement and for why every writer of this file must come through
     * this function.
     */
    store_load(path, &doc);
    if (doc != NULL) return doc;

    /*
     * Missing (the normal first run) or unreadable. A file damaged beyond
     * backup recovery lands here too: the user has already been told, and
     * refusing to save would leave the program unable to persist anything.
     */
    return store_create();
}

/* ------------------------------------------------------------------ */
/* Writing files                                                       */
/* ------------------------------------------------------------------ */

BOOL store_backup_path(const wchar_t *path, int n, wchar_t *out, size_t out_cch)
{
    if (path == NULL || out == NULL || n < 0 || n >= STORE_BACKUP_COUNT) return FALSE;

    /* "<path>.bak" for the newest, then ".bak.1", ".bak.2" for older ones.
     * Suffixes rather than timestamps: no clock to read, no collisions, and
     * the set is trivially enumerable. */
    if (n == 0) {
        _snwprintf_s(out, out_cch, _TRUNCATE, L"%ls.bak", path);
    } else {
        _snwprintf_s(out, out_cch, _TRUNCATE, L"%ls.bak.%d", path, n);
    }
    return TRUE;
}

static void rotate_backups(const wchar_t *path)
{
    wchar_t from[512], to[512];

    for (int i = STORE_BACKUP_COUNT - 1; i >= 1; --i) {
        if (!store_backup_path(path, i - 1, from, 512)) continue;
        if (!store_backup_path(path, i, to, 512)) continue;
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }
}

static BOOL write_all(HANDLE h, const char *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        DWORD chunk = (DWORD)((len - written) > 65536 ? 65536 : (len - written));
        DWORD got = 0;
        if (!WriteFile(h, data + written, chunk, &got, NULL) || got == 0) return FALSE;
        written += got;
    }
    return TRUE;
}

/*
 * Size the serialization buffer from what the document actually holds.
 * Not from sizeof(StoreDocument): that is ~26 MB, and malloc'ing it on every
 * save would be wasteful for the few kilobytes a real config needs.
 */
static size_t serialized_capacity(const StoreDocument *doc)
{
    size_t cap = 4096;
    for (int s = 0; s < doc->count; ++s) {
        const StoreSection *sec = doc->sections[s];
        if (sec == NULL) continue;
        cap += 64;                                    /* [name]\r\n */
        cap += (size_t)sec->count * (STORE_KEY_MAX + STORE_VALUE_MAX + 8);
        cap += 4;                                     /* blank separator */
    }
    return cap;
}

BOOL store_save(const wchar_t *path, StoreDocument *doc)
{
    if (path == NULL || doc == NULL) return FALSE;

    /*
     * Stamp the checksum into the LIVE document, then serialize.
     *
     * An earlier revision copied the whole document by value - 26 MB on the
     * stack - which overflowed immediately. Mutating in place is correct
     * here: the checksum is a derived field, and the document the caller
     * holds should end up matching what was written.
     */
    store_stamp_checksum(doc);

    const size_t cap = serialized_capacity(doc);
    char *buf = (char *)malloc(cap);
    if (buf == NULL) return FALSE;

    size_t n = 0;
    for (int s = 0; s < doc->count; ++s) {
        const StoreSection *sec = doc->sections[s];
        if (sec == NULL) continue;

        int wrote = _snprintf_s(buf + n, cap - n, _TRUNCATE, "[%s]\r\n", sec->name);
        if (wrote < 0) { free(buf); return FALSE; }
        n += (size_t)wrote;

        for (int k = 0; k < sec->count; ++k) {
            const StoreKV *kv = &sec->items[k];
            wrote = _snprintf_s(buf + n, cap - n, _TRUNCATE, "%s=%s\r\n", kv->key, kv->value);
            if (wrote < 0) { free(buf); return FALSE; }
            n += (size_t)wrote;
        }

        wrote = _snprintf_s(buf + n, cap - n, _TRUNCATE, "\r\n");
        if (wrote < 0) { free(buf); return FALSE; }
        n += (size_t)wrote;
    }

    /* ---- write to a temp file, then swap it in -------------------- */
    wchar_t tmp[512];
    _snwprintf_s(tmp, 512, _TRUNCATE, L"%ls.tmp", path);

    BOOL ok = FALSE;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ok = write_all(h, buf, n);

        if (ok) {
            /* Force the bytes to disk BEFORE the rename. Without this the
             * rename can be committed while the data is still in the cache,
             * and a power loss then leaves a correctly-named empty file. */
            ok = FlushFileBuffers(h);
        }
        CloseHandle(h);

        if (ok) {
            rotate_backups(path);

            /* Keep a copy of the file we are about to replace. */
            wchar_t bak[512];
            if (store_backup_path(path, 0, bak, 512)) {
                CopyFileW(path, bak, FALSE);   /* fails harmlessly on first run */
            }

            ok = MoveFileExW(tmp, path,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }

        if (!ok) DeleteFileW(tmp);   /* never leave a stray temp behind */
    }

    free(buf);
    return ok;
}

void store_delete_all(const wchar_t *path)
{
    if (path == NULL) return;

    DeleteFileW(path);

    wchar_t tmp[512];
    _snwprintf_s(tmp, 512, _TRUNCATE, L"%ls.tmp", path);
    DeleteFileW(tmp);

    for (int i = 0; i < STORE_BACKUP_COUNT; ++i) {
        wchar_t bak[512];
        if (store_backup_path(path, i, bak, 512)) DeleteFileW(bak);
    }
}
