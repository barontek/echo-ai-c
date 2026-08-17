/*
 * tui_keys.h - keymap model: key-string parsing, command bindings with
 * key overrides, and the leader-chord engine. Pure model (no terminal
 * I/O): the UI layer converts input events to TuiKeyStroke and feeds them
 * to tui_keymap_dispatch(), which returns matched command names. The
 * command registry (tui_command) owns what a name means; the keymap owns
 * which keys reach it.
 * Depends on: stdlib.
 */

#ifndef ECHO_TUI_KEYS_H
#define ECHO_TUI_KEYS_H

#include <stdint.h>
#include <stddef.h>

/* Canonical key identifiers. Values are arbitrary and unrelated to any
 * terminal encoding; the UI layer maps its input codes onto these.
 * Codepoints (ASCII/Unicode) are represented directly as their numeric
 * value, so a stroke's id is either a codepoint or one of these. */
typedef enum {
    TUI_KEYID_NONE = 0,
    TUI_KEYID_LEADER,   /* the leader token inside a chord spec, e.g. "<leader>m" */
    TUI_KEYID_ENTER,
    TUI_KEYID_ESC,
    TUI_KEYID_TAB,
    TUI_KEYID_BACKSPACE,
    TUI_KEYID_DEL,
    TUI_KEYID_INSERT,
    TUI_KEYID_HOME,
    TUI_KEYID_END,
    TUI_KEYID_PGUP,
    TUI_KEYID_PGDOWN,
    TUI_KEYID_UP,
    TUI_KEYID_DOWN,
    TUI_KEYID_LEFT,
    TUI_KEYID_RIGHT,
    TUI_KEYID_SPACE,
    TUI_KEYID_F1,
    TUI_KEYID_F2,
    TUI_KEYID_F3,
    TUI_KEYID_F4,
    TUI_KEYID_F5,
    TUI_KEYID_F6,
    TUI_KEYID_F7,
    TUI_KEYID_F8,
    TUI_KEYID_F9,
    TUI_KEYID_F10,
    TUI_KEYID_F11,
    TUI_KEYID_F12,
    TUI_KEYID_SCROLL_UP,
    TUI_KEYID_SCROLL_DOWN
} TuiKeyId;

/* One normalized keystroke. Modifiers are 0/1 booleans; id is a Unicode
 * codepoint or a TuiKeyId value. */
typedef struct {
    uint32_t id;
    unsigned ctrl;
    unsigned shift;
    unsigned alt;
    unsigned meta;
    unsigned super;
} TuiKeyStroke;

/* Fixed caps keep the keymap allocation-free: bounded command set, at
 * most TUI_KEYSEQ_MAX_ALT alternative key specs per command (e.g.
 * "ctrl+c,ctrl+d,leader+q"), at most TUI_KEYSEQ_MAX_KEYS strokes per
 * chord. A spec beyond these caps fails to parse rather than truncating. */
#define TUI_KEYMAP_MAX_BINDINGS 128
#define TUI_KEYSEQ_MAX_ALT 4
#define TUI_KEYSEQ_MAX_KEYS 4
#define TUI_KEY_NAME_MAX 48
#define TUI_KEY_CATEGORY_MAX 24
#define TUI_KEY_DESC_MAX 96
#define TUI_KEY_SPEC_MAX 64

typedef struct {
    char name[TUI_KEY_NAME_MAX];       /* command name, e.g. "session.list" */
    char category[TUI_KEY_CATEGORY_MAX];
    char desc[TUI_KEY_DESC_MAX];
    char keys[TUI_KEY_SPEC_MAX];       /* original spec for display */
    int enabled;                       /* 0 disables the binding */
    int alt_count;
    int alt_lens[TUI_KEYSEQ_MAX_ALT];  /* strokes per alternative */
    TuiKeyStroke alts[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
} TuiKeyBinding;

typedef struct TuiKeymap TuiKeymap;

/* Result of a dispatch. */
typedef enum {
    TUI_KEYMAP_CMD,      /* matched a command; name_out is set */
    TUI_KEYMAP_LEADER,   /* the leader was armed or a chord consumed */
    TUI_KEYMAP_NOMATCH   /* no binding matched; caller treats as free input */
} TuiKeymapResult;

/**
 * tui_keymap_create - allocate an empty keymap
 *
 * The default leader is "ctrl+x" with a 2000 ms timeout; override with
 * tui_keymap_set_leader() / tui_keymap_set_leader_timeout() before use.
 *
 * Return: caller-owned TuiKeymap, or NULL on allocation failure. Release
 *   with tui_keymap_destroy().
 */
TuiKeymap *tui_keymap_create(void);

/**
 * tui_keymap_destroy - release a keymap
 * @km: keymap to release, or NULL (no-op).
 *
 * Return: void.
 */
void tui_keymap_destroy(TuiKeymap *km);

/**
 * tui_keymap_register - add a binding (copy) to the keymap
 * @km: keymap; non-NULL.
 * @b: binding to copy; non-NULL.
 *
 * The binding's key spec is parsed here; an unparseable spec fails the
 * registration so a typo never silently binds nothing. Re-registering an
 * existing name replaces its key spec and metadata but keeps its enabled
 * state (use tui_keymap_bind()/tui_keymap_disable() for overrides).
 *
 * Return: 0 on success, -1 on a parse error or a full table.
 */
int tui_keymap_register(TuiKeymap *km, const TuiKeyBinding *b);

/**
 * tui_keymap_bind - override an existing binding's keys
 * @km: keymap; non-NULL.
 * @name: command name; non-NULL.
 * @keys: new key spec, e.g. "ctrl+p" or "none"/"" to disable; non-NULL.
 *
 * Re-parses and replaces the keys of the named binding. The name must
 * already be registered; unknown names fail with -1 (callers log and
 * skip per config convention). "none" or "" disables the binding.
 *
 * Return: 0 on success, -1 when the name is unknown or the spec is bad.
 */
int tui_keymap_bind(TuiKeymap *km, const char *name, const char *keys);

/**
 * tui_keymap_disable - disable a binding by name
 * @km: keymap; non-NULL.
 * @name: command name; non-NULL.
 *
 * Return: 0 on success, -1 when the name is unknown.
 */
int tui_keymap_disable(TuiKeymap *km, const char *name);

/**
 * tui_keymap_set_leader - set the leader key
 * @km: keymap; non-NULL.
 * @spec: key spec, e.g. "ctrl+x"; non-NULL.
 *
 * Return: 0 on success, -1 on a parse error (keymap keeps its old leader).
 */
int tui_keymap_set_leader(TuiKeymap *km, const char *spec);

/**
 * tui_keymap_set_leader_timeout - set the leader chord timeout
 * @km: keymap; non-NULL.
 * @ms: timeout in milliseconds (>= 1).
 *
 * Return: void.
 */
void tui_keymap_set_leader_timeout(TuiKeymap *km, uint64_t ms);

/**
 * tui_keymap_leader_active - is a leader chord awaiting its next key?
 * @km: keymap; non-NULL.
 *
 * Return: 1 while armed, 0 otherwise.
 */
int tui_keymap_leader_active(const TuiKeymap *km);

/**
 * tui_keymap_leader_clear - disarm any pending leader chord
 * @km: keymap; non-NULL.
 *
 * Call when the leader should be cancelled (Esc pressed, focus lost).
 *
 * Return: void.
 */
void tui_keymap_leader_clear(TuiKeymap *km);

/**
 * tui_keymap_leader_expired - has an armed leader timed out?
 * @km: keymap; non-NULL.
 * @now_ms: monotonic clock in milliseconds.
 *
 * If an armed leader's deadline passed, clears it and returns 1. The
 * caller uses this to surface a "leader timeout" hint in the UI.
 *
 * Return: 1 when the armed leader just expired (now disarmed), else 0.
 */
int tui_keymap_leader_expired(TuiKeymap *km, uint64_t now_ms);

/**
 * tui_keymap_dispatch - resolve one keystroke to a command
 * @km: keymap; non-NULL.
 * @stroke: normalized keystroke; non-NULL.
 * @now_ms: monotonic clock in milliseconds.
 * @name_out: receives the matched command name (borrowed, valid until the
 *   keymap mutates); set only on TUI_KEYMAP_CMD. Must be non-NULL.
 *
 * A stroke equal to the leader arms the chord and returns TUI_KEYMAP_LEADER.
 * While armed, the next stroke is matched against leader chords; a match
 * returns the command and disarms. A non-matching stroke disarms and is
 * retried as a fresh single-key press (so stray keys are never dropped).
 * Single-key bindings then match in registration order (last wins for
 * duplicates). An expired leader is disarmed before anything else.
 *
 * Return: TUI_KEYMAP_CMD, TUI_KEYMAP_LEADER, or TUI_KEYMAP_NOMATCH.
 */
TuiKeymapResult tui_keymap_dispatch(TuiKeymap *km, const TuiKeyStroke *stroke,
                                    uint64_t now_ms, const char **name_out);

/* Visitor for tui_keymap_walk; receives one enabled-or-disabled binding. */
typedef void (*TuiKeymapWalkFn)(void *ud, const TuiKeyBinding *b);

/**
 * tui_keymap_walk - visit every registered binding in registration order
 * @km: keymap; non-NULL.
 * @fn: visitor; non-NULL.
 * @ud: passed through to fn.
 *
 * Feeds every binding, enabled or not, so callers can render help/keybind
 * dumps that mark disabled entries.
 *
 * Return: void.
 */
void tui_keymap_walk(const TuiKeymap *km, TuiKeymapWalkFn fn, void *ud);

/**
 * tui_keymap_binding - look up a binding by name
 * @km: keymap; non-NULL.
 * @name: command name; non-NULL.
 *
 * Return: borrowed binding, or NULL when the name is unknown.
 */
const TuiKeyBinding *tui_keymap_binding(const TuiKeymap *km, const char *name);

/**
 * tui_keymap_count - number of registered bindings
 * @km: keymap; non-NULL.
 *
 * Return: the binding count.
 */
int tui_keymap_count(const TuiKeymap *km);

/**
 * tui_keymap_at - the binding at an index
 * @km: keymap; non-NULL.
 * @idx: 0-based index; must be < tui_keymap_count().
 *
 * Return: borrowed binding, or NULL when @idx is out of range.
 */
const TuiKeyBinding *tui_keymap_at(const TuiKeymap *km, int idx);

/**
 * tui_key_parse - parse one key spec into strokes
 * @spec: key spec, e.g. "ctrl+x", "shift+tab", "leader+m", "f2",
 *   "pgdown", or comma-separated alternatives; non-NULL.
 * @alt_count: out-param receiving the number of alternatives (>= 1).
 * @lens: out-param receiving each alternative's stroke count.
 * @strokes: out-param receiving the strokes, row per alternative.
 *
 * Modifier order is free ("ctrl+shift+x"); names are case-insensitive.
 * Recognized key names: enter/return, esc/escape, tab, backspace,
 * del/delete, insert, home, end, pgup/pageup, pgdown/pagedown, up, down,
 * left, right, space, f1..f12, and the leader token ("leader" or
 * "<leader>"). A lone character is that codepoint. Whitespace is ignored
 * around tokens. More than TUI_KEYSEQ_MAX_ALT alternatives or more than
 * TUI_KEYSEQ_MAX_KEYS strokes per alternative fails.
 *
 * Return: 0 on success, -1 on an unknown token or an overlong spec.
 */
int tui_key_parse(const char *spec, int *alt_count, int *lens,
                  TuiKeyStroke strokes[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS]);

/**
 * tui_key_stroke_equal - do two strokes mean the same keypress?
 * @a: stroke; non-NULL.
 * @b: stroke; non-NULL.
 *
 * Return: 1 when id and every modifier match, else 0.
 */
int tui_key_stroke_equal(const TuiKeyStroke *a, const TuiKeyStroke *b);

/**
 * tui_key_stroke_normalize - canonicalize a stroke's case in place
 * @s: stroke; non-NULL.
 *
 * Terminal layers report the shifted glyph as the key id (Ctrl+P arrives
 * as id 'P' with the ctrl bit; Shift+P as id 'P' with the shift bit).
 * Normalizing lowers ASCII letters and records shift on the uppercase
 * form (unless ctrl is set, where the shift bit is unreliable). The
 * parser applies this to specs and the input layer to events, so "ctrl+p"
 * and a pressed Ctrl+P compare equal.
 *
 * Return: void.
 */
void tui_key_stroke_normalize(TuiKeyStroke *s);

/**
 * tui_key_stroke_to_string - render a stroke back to a spec string
 * @s: stroke; non-NULL.
 * @out: caller-owned buffer; non-NULL.
 * @cap: capacity of out; must be >= 1.
 *
 * "ctrl+x", "shift+tab", "f2", "enter", "leader+m". The leader token is
 * rendered as "leader+m" (parse accepts the "<leader>m" spelling too).
 *
 * Return: bytes written excluding the trailing NUL.
 */
size_t tui_key_stroke_to_string(const TuiKeyStroke *s, char *out, size_t cap);

#endif /* ECHO_TUI_KEYS_H */