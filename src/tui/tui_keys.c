/*
 * tui_keys.c - keymap model implementation: key-string parsing, the
 * binding table, and the leader-chord engine. Purely string/map logic so
 * it is fully unit-testable; the UI layer (tui.c) owns input-event
 * capture and converting events to TuiKeyStroke.
 * Depends on: tui_keys.h, stdlib, ctype.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "tui_keys.h"

/* Default leader and chord timeout, matching opencode's defaults. */
#define TUI_LEADER_DEFAULT "ctrl+x"
#define TUI_LEADER_TIMEOUT_MS 2000u

struct TuiKeymap {
    TuiKeyBinding bindings[TUI_KEYMAP_MAX_BINDINGS];
    int count;
    TuiKeyStroke leader;
    char leader_spec[TUI_KEY_SPEC_MAX];
    uint64_t leader_timeout;   /* ms */
    int leader_armed;
    uint64_t leader_deadline;  /* monotonic ms */
};

/* ---- key parsing ---- */

static int name_to_keyid(const char *name)
{
    struct { const char *name; int id; } map[] = {
        { "enter",  TUI_KEYID_ENTER },
        { "return", TUI_KEYID_ENTER },
        { "esc",    TUI_KEYID_ESC },
        { "escape", TUI_KEYID_ESC },
        { "tab",    TUI_KEYID_TAB },
        { "backspace", TUI_KEYID_BACKSPACE },
        { "del",    TUI_KEYID_DEL },
        { "delete", TUI_KEYID_DEL },
        { "insert", TUI_KEYID_INSERT },
        { "ins",    TUI_KEYID_INSERT },
        { "home",   TUI_KEYID_HOME },
        { "end",    TUI_KEYID_END },
        { "pgup",   TUI_KEYID_PGUP },
        { "pageup", TUI_KEYID_PGUP },
        { "pgdown", TUI_KEYID_PGDOWN },
        { "pagedown", TUI_KEYID_PGDOWN },
        { "up",     TUI_KEYID_UP },
        { "down",   TUI_KEYID_DOWN },
        { "left",   TUI_KEYID_LEFT },
        { "right",  TUI_KEYID_RIGHT },
        { "space",  TUI_KEYID_SPACE },
        { "scrollup",   TUI_KEYID_SCROLL_UP },
        { "scrolldown", TUI_KEYID_SCROLL_DOWN },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].name) == 0)
            return map[i].id;
    /* f1..f12 */
    if (name[0] == 'f' || name[0] == 'F')
    {
        char *end = NULL;
        long n = strtol(name + 1, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 12)
            return TUI_KEYID_F1 + (int)(n - 1);
    }
    return -1;
}

/* Parse one bare key token (no modifiers) into id. The token may carry a
 * stray '+' from a trailing modifier split; handled by the caller. */
static int parse_key_token(const char *tok, size_t len, uint32_t *id)
{
    if (len == 0) return -1;
    /* leader token: accept "leader" and "<leader>" */
    if ((len == 6 && strncmp(tok, "leader", 6) == 0) ||
        (len == 8 && strncmp(tok, "<leader>", 8) == 0))
    {
        *id = TUI_KEYID_LEADER;
        return 0;
    }
    if (len == 1 && tok[0] == '<') return -1; /* malformed bracket token */

    char name[24];
    if (len >= sizeof(name)) return -1;
    memcpy(name, tok, len);
    name[len] = '\0';
    for (size_t i = 0; i < len; i++) name[i] = (char)tolower((unsigned char)name[i]);

    int kid = name_to_keyid(name);
    if (kid >= 0)
    {
        *id = (uint32_t)kid;
        return 0;
    }
    if (len == 1)
    {
        /* keep the original case so the caller's normalize step can
         * record shift on an uppercase letter */
        *id = (unsigned char)tok[0];
        return 0;
    }
    return -1;
}

/* Parse one '+'-joined token run into strokes. Modifier tokens (ctrl,
 * shift, ...) attach to the current stroke's key; a key token (a key
 * name, the leader token, or a single character) finalizes the current
 * stroke and starts a new one — so "ctrl+x" is one stroke while
 * "leader+q" is a two-stroke chord and "ctrl+x ctrl+y" is two strokes.
 * Returns the stroke count, or -1 on an unknown token. */
static int parse_stroke_run(const char *run, TuiKeyStroke *out, int cap)
{
    /* "<leader>X" concatenates the leader token with its continuation key;
     * split it into two strokes before the '+'/modifier pass. */
    if (strncmp(run, "<leader>", 8) == 0 && run[8] != '\0')
    {
        if (cap < 2) return -1;
        out[0].id = TUI_KEYID_LEADER;
        int nr = parse_stroke_run(run + 8, out + 1, cap - 1);
        if (nr < 0) return -1;
        return 1 + nr;
    }

    char buf[64];
    size_t len = strlen(run);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, run, len + 1);

    char *parts[8];
    int nparts = 0;
    char *save = NULL;
    for (char *p = strtok_r(buf, "+", &save); p; p = strtok_r(NULL, "+", &save))
    {
        if (nparts >= 8) return -1;
        parts[nparts++] = p;
    }
    if (nparts == 0) return -1;

    int nkeys = 0;
    TuiKeyStroke cur;
    memset(&cur, 0, sizeof(cur));
    for (int i = 0; i < nparts; i++)
    {
        const char *tok = parts[i];
        char mod[24]; /* long enough for "backspace", "pagedown", ... */
        size_t tlen = strlen(tok);
        if (tlen >= sizeof(mod)) return -1;
        memcpy(mod, tok, tlen + 1);
        for (size_t k = 0; k < tlen; k++)
            mod[k] = (char)tolower((unsigned char)mod[k]);
        if (strcmp(mod, "ctrl") == 0) { cur.ctrl = 1; continue; }
        if (strcmp(mod, "shift") == 0) { cur.shift = 1; continue; }
        if (strcmp(mod, "alt") == 0) { cur.alt = 1; continue; }
        if (strcmp(mod, "meta") == 0) { cur.meta = 1; continue; }
        if (strcmp(mod, "super") == 0) { cur.super = 1; continue; }

        /* a key token: finalize any pending modifiers */
        uint32_t id;
        if (parse_key_token(tok, strlen(tok), &id) != 0) return -1;
        if (nkeys >= cap) return -1;
        cur.id = id;
        tui_key_stroke_normalize(&cur);
        out[nkeys++] = cur;
        memset(&cur, 0, sizeof(cur));
    }
    if (nkeys == 0) return -1; /* modifiers with no key */
    return nkeys;
}

int tui_key_parse(const char *spec, int *alt_count, int *lens,
                  TuiKeyStroke strokes[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS])
{
    if (!spec || !alt_count || !lens || !strokes) return -1;

    char buf[256];
    if (strlen(spec) >= sizeof(buf)) return -1;
    strlcpy(buf, spec, sizeof(buf));

    int alts = 0;
    char *save = NULL;
    for (char *p = strtok_r(buf, ",", &save); p && alts < TUI_KEYSEQ_MAX_ALT;
         p = strtok_r(NULL, ",", &save))
    {
        /* trim whitespace around the alternative */
        char *s = p;
        while (isspace((unsigned char)*s)) s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)*(e - 1))) e--;
        *e = '\0';
        if (s[0] == '\0') continue;

        /* a space-separated alternative is an explicit multi-key chord */
        TuiKeyStroke tmp[TUI_KEYSEQ_MAX_KEYS];
        int nkeys = 0;
        char *save2 = NULL;
        for (char *tok = strtok_r(s, " ", &save2); tok;
             tok = strtok_r(NULL, " ", &save2))
        {
            TuiKeyStroke run[TUI_KEYSEQ_MAX_KEYS];
            int nr = parse_stroke_run(tok, run, TUI_KEYSEQ_MAX_KEYS);
            if (nr < 0) return -1;
            for (int k = 0; k < nr; k++)
            {
                if (nkeys >= TUI_KEYSEQ_MAX_KEYS) return -1;
                tmp[nkeys++] = run[k];
            }
        }
        if (nkeys == 0) return -1;
        lens[alts] = nkeys;
        for (int k = 0; k < nkeys; k++) strokes[alts][k] = tmp[k];
        alts++;
    }
    if (alts == 0) return -1;
    *alt_count = alts;
    return 0;
}

int tui_key_stroke_equal(const TuiKeyStroke *a, const TuiKeyStroke *b)
{
    if (!a || !b) return 0;
    return a->id == b->id && a->ctrl == b->ctrl && a->shift == b->shift &&
           a->alt == b->alt && a->meta == b->meta && a->super == b->super;
}

void tui_key_stroke_normalize(TuiKeyStroke *s)
{
    if (!s) return;
    if (s->id >= 'A' && s->id <= 'Z')
    {
        s->id = s->id - 'A' + 'a';
        if (!s->ctrl)
            s->shift = 1;
    }
}

static const char *keyid_to_name(uint32_t id)
{
    struct { uint32_t id; const char *name; } map[] = {
        { TUI_KEYID_ENTER, "enter" },
        { TUI_KEYID_ESC, "esc" },
        { TUI_KEYID_TAB, "tab" },
        { TUI_KEYID_BACKSPACE, "backspace" },
        { TUI_KEYID_DEL, "del" },
        { TUI_KEYID_INSERT, "insert" },
        { TUI_KEYID_HOME, "home" },
        { TUI_KEYID_END, "end" },
        { TUI_KEYID_PGUP, "pgup" },
        { TUI_KEYID_PGDOWN, "pgdown" },
        { TUI_KEYID_UP, "up" },
        { TUI_KEYID_DOWN, "down" },
        { TUI_KEYID_LEFT, "left" },
        { TUI_KEYID_RIGHT, "right" },
        { TUI_KEYID_SPACE, "space" },
        { TUI_KEYID_SCROLL_UP, "scrollup" },
        { TUI_KEYID_SCROLL_DOWN, "scrolldown" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].id == id)
            return map[i].name;
    if (id >= TUI_KEYID_F1 && id <= TUI_KEYID_F12)
    {
        static char fbuf[4];
        snprintf(fbuf, sizeof(fbuf), "f%u", (unsigned)(id - TUI_KEYID_F1 + 1)); // NOLINT(cert-err33-c)
        return fbuf;
    }
    return NULL;
}

size_t tui_key_stroke_to_string(const TuiKeyStroke *s, char *out, size_t cap)
{
    if (!s || !out || cap == 0) return 0;
    size_t o = 0;
    if (s->ctrl)   o += (size_t)snprintf(out + o, cap - o, "ctrl+");
    if (s->shift)  o += (size_t)snprintf(out + o, cap - o, "shift+");
    if (s->alt)    o += (size_t)snprintf(out + o, cap - o, "alt+");
    if (s->meta)   o += (size_t)snprintf(out + o, cap - o, "meta+");
    if (s->super)  o += (size_t)snprintf(out + o, cap - o, "super+");
    const char *n = keyid_to_name(s->id);
    if (n)
        o += (size_t)snprintf(out + o, cap - o, "%s", n);
    else if (s->id >= 0x20 && s->id < 0x110000 && s->id != 0x7f)
        o += (size_t)snprintf(out + o, cap - o, "%c", (int)s->id);
    else
        o += (size_t)snprintf(out + o, cap - o, "key%u", (unsigned)s->id);
    return o;
}

/* ---- keymap ---- */

TuiKeymap *tui_keymap_create(void)
{
    TuiKeymap *km = calloc(1, sizeof(TuiKeymap));
    if (!km) return NULL;
    if (tui_keymap_set_leader(km, TUI_LEADER_DEFAULT) != 0)
    {
        free(km);
        return NULL;
    }
    km->leader_timeout = TUI_LEADER_TIMEOUT_MS;
    return km;
}

void tui_keymap_destroy(TuiKeymap *km)
{
    free(km);
}

/* Bounded copy that always NUL-terminates; the source is truncated to
 * fit. Fields are pre-zeroed so a truncation leaves a valid string. */
static void copy_field(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src); // NOLINT(cert-err33-c)
}

static int binding_index(const TuiKeymap *km, const char *name)
{
    for (int i = 0; i < km->count; i++)
        if (strcmp(km->bindings[i].name, name) == 0)
            return i;
    return -1;
}

int tui_keymap_register(TuiKeymap *km, const TuiKeyBinding *b)
{
    if (!km || !b) return -1;

    int idx = binding_index(km, b->name);
    if (idx >= 0)
    {
        TuiKeyBinding *dst = &km->bindings[idx];
        int enabled = dst->enabled;
        TuiKeyBinding copy = *b;
        copy.enabled = enabled; /* re-register keeps its override state */
        *dst = copy;
        return 0;
    }
    if (km->count >= TUI_KEYMAP_MAX_BINDINGS) return -1;

    int alt_count;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke strokes[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    if (tui_key_parse(b->keys, &alt_count, lens, strokes) != 0) return -1;

    TuiKeyBinding *dst = &km->bindings[km->count];
    memset(dst, 0, sizeof(*dst));
    copy_field(dst->name, sizeof(dst->name), b->name);
    copy_field(dst->category, sizeof(dst->category), b->category);
    copy_field(dst->desc, sizeof(dst->desc), b->desc);
    copy_field(dst->keys, sizeof(dst->keys), b->keys);
    dst->enabled = 1;
    dst->alt_count = alt_count;
    for (int a = 0; a < alt_count; a++)
    {
        dst->alt_lens[a] = lens[a];
        for (int k = 0; k < lens[a]; k++) dst->alts[a][k] = strokes[a][k];
    }
    km->count++;
    return 0;
}

int tui_keymap_bind(TuiKeymap *km, const char *name, const char *keys)
{
    if (!km || !name || !keys) return -1;
    int idx = binding_index(km, name);
    if (idx < 0) return -1;

    if (keys[0] == '\0' || strcmp(keys, "none") == 0)
    {
        km->bindings[idx].enabled = 0;
        return 0;
    }
    int alt_count;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke strokes[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    if (tui_key_parse(keys, &alt_count, lens, strokes) != 0) return -1;

    TuiKeyBinding *dst = &km->bindings[idx];
    copy_field(dst->keys, sizeof(dst->keys), keys);
    dst->alt_count = alt_count;
    for (int a = 0; a < alt_count; a++)
    {
        dst->alt_lens[a] = lens[a];
        for (int k = 0; k < lens[a]; k++) dst->alts[a][k] = strokes[a][k];
    }
    dst->enabled = 1;
    return 0;
}

int tui_keymap_disable(TuiKeymap *km, const char *name)
{
    if (!km || !name) return -1;
    int idx = binding_index(km, name);
    if (idx < 0) return -1;
    km->bindings[idx].enabled = 0;
    return 0;
}

int tui_keymap_set_leader(TuiKeymap *km, const char *spec)
{
    if (!km || !spec) return -1;
    int alt_count;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke strokes[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    if (tui_key_parse(spec, &alt_count, lens, strokes) != 0) return -1;
    if (alt_count != 1 || lens[0] != 1) return -1; /* leader is one key */
    if (strokes[0][0].id == TUI_KEYID_LEADER) return -1; /* no recursion */
    km->leader = strokes[0][0];
    copy_field(km->leader_spec, sizeof(km->leader_spec), spec);
    return 0;
}

void tui_keymap_set_leader_timeout(TuiKeymap *km, uint64_t ms)
{
    if (km) km->leader_timeout = ms > 0 ? ms : 1;
}

int tui_keymap_leader_active(const TuiKeymap *km)
{
    return km && km->leader_armed;
}

void tui_keymap_leader_clear(TuiKeymap *km)
{
    if (km) km->leader_armed = 0;
}

int tui_keymap_leader_expired(TuiKeymap *km, uint64_t now_ms)
{
    if (!km || !km->leader_armed) return 0;
    if (now_ms >= km->leader_deadline)
    {
        km->leader_armed = 0;
        return 1;
    }
    return 0;
}

TuiKeymapResult tui_keymap_dispatch(TuiKeymap *km, const TuiKeyStroke *stroke,
                                    uint64_t now_ms, const char **name_out)
{
    if (!km || !stroke || !name_out) return TUI_KEYMAP_NOMATCH;
    *name_out = NULL;

    /* normalize a defensive copy so raw (un-normalized) event strokes
     * still match; the input layer normalizes too, this is belt-and-braces */
    TuiKeyStroke norm = *stroke;
    tui_key_stroke_normalize(&norm);

    if (km->leader_armed)
    {
        (void)tui_keymap_leader_expired(km, now_ms);
        if (km->leader_armed)
        {
            /* try to complete a leader chord */
            for (int i = 0; i < km->count; i++)
            {
                const TuiKeyBinding *b = &km->bindings[i];
                if (!b->enabled) continue;
                for (int a = 0; a < b->alt_count; a++)
                {
                    if (b->alt_lens[a] < 2) continue;
                    if (b->alts[a][0].id != TUI_KEYID_LEADER) continue;
                    if (b->alt_lens[a] != 2) continue; /* 2-key chords for now */
                    if (tui_key_stroke_equal(&b->alts[a][1], &norm))
                    {
                        km->leader_armed = 0;
                        *name_out = b->name;
                        return TUI_KEYMAP_CMD;
                    }
                }
            }
            /* no chord matched: disarm and fall through to a fresh press */
            km->leader_armed = 0;
        }
    }

    if (tui_key_stroke_equal(&norm, &km->leader))
    {
        km->leader_armed = 1;
        km->leader_deadline = now_ms + km->leader_timeout;
        return TUI_KEYMAP_LEADER;
    }

    for (int i = 0; i < km->count; i++)
    {
        const TuiKeyBinding *b = &km->bindings[i];
        if (!b->enabled) continue;
        for (int a = 0; a < b->alt_count; a++)
        {
            if (b->alt_lens[a] != 1) continue; /* single-key only here */
            if (tui_key_stroke_equal(&b->alts[a][0], &norm))
            {
                *name_out = b->name;
                return TUI_KEYMAP_CMD;
            }
        }
    }
    return TUI_KEYMAP_NOMATCH;
}

void tui_keymap_walk(const TuiKeymap *km, TuiKeymapWalkFn fn, void *ud)
{
    if (!km || !fn) return;
    for (int i = 0; i < km->count; i++)
        fn(ud, &km->bindings[i]);
}

const TuiKeyBinding *tui_keymap_binding(const TuiKeymap *km, const char *name)
{
    if (!km || !name) return NULL;
    int idx = binding_index(km, name);
    return idx < 0 ? NULL : &km->bindings[idx];
}

int tui_keymap_count(const TuiKeymap *km)
{
    return km ? km->count : 0;
}

const TuiKeyBinding *tui_keymap_at(const TuiKeymap *km, int idx)
{
    if (!km || idx < 0 || idx >= km->count) return NULL;
    return &km->bindings[idx];
}
