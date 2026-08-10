/*
 * html_extract.c - readable-text extraction from raw HTML (Crawl4AI-style
 * pipeline: tag classification, frame scoring, entity decoding, citation
 * footer, head-biased truncation) plus Content-Type dispatch for fetched
 * web bytes. Depends on: string_utils.h, libc (ctype/math/stdio/stdlib).
 */

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_extract.h"
#include "string_utils.h"

/* Fault injection for allocation-failure tests (AGENTS.md section 11):
 * only this translation unit sees the macro, so production builds are
 * unaffected. */
#ifdef HTML_EXTRACT_TEST
static int extract_alloc_counter = 0;
static int extract_alloc_fail_at = -1;

void html_extract_test_set_alloc_fail(int nth_allocation)
{
    extract_alloc_counter = 0;
    extract_alloc_fail_at = nth_allocation;
}

static void *test_realloc(void *ptr, size_t size)
{
    extract_alloc_counter++;
    if (extract_alloc_counter == extract_alloc_fail_at) return NULL;
    return realloc(ptr, size);
}

static void *test_malloc(size_t size)
{
    extract_alloc_counter++;
    if (extract_alloc_counter == extract_alloc_fail_at) return NULL;
    return malloc(size);
}

#define malloc test_malloc
#define realloc test_realloc
#endif

#define MAX_FRAME_DEPTH 64
#define MAX_LINKS 20
#define MAX_HREF_LEN 2048
#define MAX_ATTR_LEN 256
#define SCORE_THRESHOLD 0.48
#define MIN_CONTENT_WORDS 4
/* Reserved room for the truncation marker; 46 covers a marker with up to
 * 33 omitted-digit counts, which far exceeds any realistic page size. */
#define MARKER_ROOM 46

/* ------------------------------------------------------------------ */
/* Tag classification (Crawl4AI's excluded/included tag sets)          */
/* ------------------------------------------------------------------ */

typedef enum {
    TAG_FRAME_ARTICLE, TAG_FRAME_SECTION, TAG_FRAME_MAIN, TAG_FRAME_DIV,
    TAG_FRAME_P, TAG_FRAME_UL, TAG_FRAME_OL, TAG_FRAME_LI, TAG_FRAME_DL,
    TAG_FRAME_DT, TAG_FRAME_DD, TAG_FRAME_BLOCKQUOTE, TAG_FRAME_PRE,
    TAG_FRAME_TABLE, TAG_FRAME_THEAD, TAG_FRAME_TBODY, TAG_FRAME_TFOOT,
    TAG_FRAME_TR, TAG_FRAME_TD, TAG_FRAME_TH, TAG_FRAME_FIGURE,
    TAG_FRAME_FIGCAPTION, TAG_FRAME_DETAILS, TAG_FRAME_SUMMARY,
    TAG_FRAME_ADDRESS, TAG_FRAME_H1, TAG_FRAME_H2, TAG_FRAME_H3, TAG_FRAME_H4,
    TAG_FRAME_H5, TAG_FRAME_H6,
    TAG_EXCL_NAV, TAG_EXCL_FOOTER, TAG_EXCL_HEADER, TAG_EXCL_ASIDE,
    TAG_EXCL_SCRIPT, TAG_EXCL_STYLE, TAG_EXCL_FORM, TAG_EXCL_IFRAME,
    TAG_EXCL_NOSCRIPT, TAG_EXCL_TEMPLATE, TAG_EXCL_SVG,
    TAG_SPECIAL_A, TAG_SPECIAL_TITLE, TAG_SPECIAL_HTML, TAG_SPECIAL_BODY,
    TAG_SPECIAL_HEAD,
    TAG_VOID_NL_BR, TAG_VOID_NL_HR,
    TAG_VOID_IMG, TAG_VOID_META, TAG_VOID_LINK, TAG_VOID_INPUT,
    TAG_VOID_AREA, TAG_VOID_BASE, TAG_VOID_COL, TAG_VOID_EMBED,
    TAG_VOID_SOURCE, TAG_VOID_TRACK, TAG_VOID_WBR,
    TAG_UNKNOWN
} TagId;

typedef struct {
    const char *name;
    TagId id;
} TagEntry;

static const TagEntry TAG_TABLE[] = {
    {"a", TAG_SPECIAL_A},       {"address", TAG_FRAME_ADDRESS},
    {"area", TAG_VOID_AREA},    {"article", TAG_FRAME_ARTICLE},
    {"aside", TAG_EXCL_ASIDE},  {"base", TAG_VOID_BASE},
    {"blockquote", TAG_FRAME_BLOCKQUOTE}, {"body", TAG_SPECIAL_BODY},
    {"br", TAG_VOID_NL_BR},     {"col", TAG_VOID_COL},
    {"dd", TAG_FRAME_DD},       {"details", TAG_FRAME_DETAILS},
    {"div", TAG_FRAME_DIV},     {"dl", TAG_FRAME_DL},
    {"dt", TAG_FRAME_DT},       {"embed", TAG_VOID_EMBED},
    {"figcaption", TAG_FRAME_FIGCAPTION}, {"figure", TAG_FRAME_FIGURE},
    {"footer", TAG_EXCL_FOOTER},{"form", TAG_EXCL_FORM},
    {"h1", TAG_FRAME_H1},       {"h2", TAG_FRAME_H2},
    {"h3", TAG_FRAME_H3},       {"h4", TAG_FRAME_H4},
    {"h5", TAG_FRAME_H5},       {"h6", TAG_FRAME_H6},
    {"head", TAG_SPECIAL_HEAD}, {"header", TAG_EXCL_HEADER},
    {"hr", TAG_VOID_NL_HR},     {"html", TAG_SPECIAL_HTML},
    {"iframe", TAG_EXCL_IFRAME},{"img", TAG_VOID_IMG},
    {"input", TAG_VOID_INPUT},  {"li", TAG_FRAME_LI},
    {"link", TAG_VOID_LINK},    {"main", TAG_FRAME_MAIN},
    {"meta", TAG_VOID_META},    {"nav", TAG_EXCL_NAV},
    {"noscript", TAG_EXCL_NOSCRIPT}, {"ol", TAG_FRAME_OL},
    {"p", TAG_FRAME_P},         {"pre", TAG_FRAME_PRE},
    {"script", TAG_EXCL_SCRIPT},{"section", TAG_FRAME_SECTION},
    {"source", TAG_VOID_SOURCE},{"span", TAG_UNKNOWN},
    {"style", TAG_EXCL_STYLE},  {"summary", TAG_FRAME_SUMMARY},
    {"svg", TAG_EXCL_SVG},      {"table", TAG_FRAME_TABLE},
    {"tbody", TAG_FRAME_TBODY}, {"td", TAG_FRAME_TD},
    {"template", TAG_EXCL_TEMPLATE}, {"tfoot", TAG_FRAME_TFOOT},
    {"th", TAG_FRAME_TH},       {"thead", TAG_FRAME_THEAD},
    {"title", TAG_SPECIAL_TITLE},{"tr", TAG_FRAME_TR},
    {"track", TAG_VOID_TRACK},  {"ul", TAG_FRAME_UL},
    {"wbr", TAG_VOID_WBR},
};

static TagId tag_lookup(const char *name, size_t n)
{
    if (n == 0 || n >= 32) return TAG_UNKNOWN;
    char buf[32];
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[n] = '\0';
    for (size_t i = 0; i < sizeof(TAG_TABLE) / sizeof(TAG_TABLE[0]); i++)
    {
        if (strcmp(buf, TAG_TABLE[i].name) == 0) return TAG_TABLE[i].id;
    }
    return TAG_UNKNOWN;
}

static int is_frame_tag(TagId tid)
{
    return tid >= TAG_FRAME_ARTICLE && tid <= TAG_FRAME_H6;
}

static int is_excluded_tag(TagId tid)
{
    return tid >= TAG_EXCL_NAV && tid <= TAG_EXCL_SVG;
}

static int is_header_tag(TagId tid)
{
    return tid >= TAG_FRAME_H1 && tid <= TAG_FRAME_H6;
}

/* Crawl4AI tag_weights; unmapped frames get the default 0.5 (div, ul, ...) */
static double tag_weight_of(TagId tid)
{
    switch (tid)
    {
    case TAG_FRAME_ARTICLE: return 1.5;
    case TAG_FRAME_SECTION: return 1.0;
    case TAG_FRAME_P:       return 1.0;
    case TAG_FRAME_H1:      return 1.2;
    case TAG_FRAME_H2:      return 1.1;
    case TAG_FRAME_H3:      return 1.0;
    case TAG_FRAME_H4:      return 0.9;
    case TAG_FRAME_H5:      return 0.8;
    case TAG_FRAME_H6:      return 0.7;
    default:                return 0.5;
    }
}

/* Crawl4AI negative_patterns: class/id containing any of these is boilerplate */
static const char *const NEG_PATTERNS[] = {
    "nav", "footer", "header", "sidebar", "ads", "comment",
    "promo", "advert", "social", "share",
};

static int has_neg_pattern(const char *s, size_t n)
{
    for (size_t p = 0; p < sizeof(NEG_PATTERNS) / sizeof(NEG_PATTERNS[0]); p++)
    {
        size_t plen = strlen(NEG_PATTERNS[p]);
        if (n < plen) continue;
        for (size_t i = 0; i + plen <= n; i++)
        {
            size_t j = 0;
            while (j < plen &&
                   tolower((unsigned char)s[i + j]) == (unsigned char)NEG_PATTERNS[p][j])
                j++;
            if (j == plen) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Growable output buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} OutBuf;

static int outbuf_reserve(OutBuf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len - 1) return -1;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t new_cap = b->cap ? b->cap * 2 : 64;
    while (new_cap < need)
    {
        if (new_cap > SIZE_MAX / 2) { new_cap = need; break; }
        new_cap *= 2;
    }
    char *new = realloc(b->data, new_cap);
    if (!new) return -1;
    b->data = new;
    b->cap = new_cap;
    return 0;
}

static int outbuf_append(OutBuf *b, const char *s, size_t n)
{
    if (outbuf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int outbuf_append_chr(OutBuf *b, char c)
{
    if (outbuf_reserve(b, 1) != 0) return -1;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

static void outbuf_truncate(OutBuf *b, size_t len)
{
    if (len >= b->len) return;
    b->len = len;
    b->data[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Text writer: entity-aware appends with whitespace collapsing        */
/* ------------------------------------------------------------------ */

typedef struct {
    OutBuf out;
    int need_newline; /* block boundary requested but not yet emitted */
    int ws_pending;   /* collapsed space seen, not yet emitted */
    int has_last;
    char last;
} Writer;

/* Request a paragraph boundary: emitted before the next text, unless a
 * kept-block's content is rolled back by truncation. */
static void writer_newline(Writer *w)
{
    w->need_newline = 1;
}

static int writer_preflush(Writer *w)
{
    if (w->need_newline)
    {
        /* never start the output with a newline (first frame / rollback) */
        if (w->out.len > 0 &&
            outbuf_append_chr(&w->out, '\n') != 0) return -1;
        w->need_newline = 0;
        w->ws_pending = 0;
        w->has_last = 1;
        w->last = '\n';
    }
    else if (w->ws_pending && w->has_last && w->last != '\n')
    {
        if (outbuf_append_chr(&w->out, ' ') != 0) return -1;
        w->ws_pending = 0;
        w->has_last = 1;
        w->last = ' ';
    }
    return 0;
}

/* Append a text fragment, collapsing runs of whitespace to a single
 * space and honoring pending block-boundary newlines. */
static int writer_text(Writer *w, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c))
        {
            /* a pending newline wins over a pending space */
            if (!w->need_newline) w->ws_pending = 1;
            continue;
        }
        if (w->need_newline)
        {
            if (w->out.len > 0 &&
                outbuf_append_chr(&w->out, '\n') != 0) return -1;
            w->need_newline = 0;
            w->ws_pending = 0;
            w->has_last = 1;
            w->last = '\n';
        }
        else if (w->ws_pending && w->has_last && w->last != '\n')
        {
            if (outbuf_append_chr(&w->out, ' ') != 0) return -1;
            w->has_last = 1;
            w->last = ' ';
        }
        w->ws_pending = 0;
        if (outbuf_append_chr(&w->out, (char)c) != 0) return -1;
        w->has_last = 1;
        w->last = (char)c;
    }
    return 0;
}

/* After truncating the output buffer, recompute the last-char state. */
static void writer_sync_state(Writer *w)
{
    if (w->out.len > 0)
    {
        w->last = w->out.data[w->out.len - 1];
        w->has_last = 1;
    }
    else
    {
        w->last = '\0';
        w->has_last = 0;
    }
    w->ws_pending = 0;
}

/* ------------------------------------------------------------------ */
/* HTML entity decoding                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    unsigned char ch;
} EntityEntry;

static const EntityEntry ENTITY_TABLE[] = {
    {"amp", '&'},    {"lt", '<'},     {"gt", '>'},
    {"quot", '"'},   {"apos", '\''},  {"nbsp", 0xA0},
    {"copy", 0xA9},  {"reg", 0xAE},   {"trade", 0x99},
    {"hellip", 0x85},{"mdash", 0x97}, {"ndash", 0x96},
    {"lsquo", 0x91}, {"rsquo", 0x92}, {"ldquo", 0x93},
    {"rdquo", 0x94}, {"middot", 0xB7},{"bull", 0x95},
    {"deg", 0xB0},   {"plusmn", 0xB1},{"times", 0xD7},
    {"divide", 0xF7},{"frac12", 0xBD},{"frac14", 0xBC},
    {"frac34", 0xBE},{"sup2", 0xB2},  {"sup3", 0xB3},
    {"para", 0xB6},  {"sect", 0xA7},  {"permil", 0x89},
    {"euro", 0x80},  {"pound", 0xA3}, {"cent", 0xA2},
    {"yen", 0xA5},   {"micro", 0xB5},
    {"eacute", 0xE9},{"agrave", 0xE0},{"ntilde", 0xF1},
    {"ccedil", 0xE7},{"szlig", 0xDF}, {"uuml", 0xFC},
    {"aacute", 0xE1},{"iacute", 0xED},{"oacute", 0xF3},
    {"uacute", 0xFA},{"auml", 0xE4},  {"igrave", 0xEC},
    {"ograve", 0xF2},{"ugrave", 0xF9},{"acirc", 0xE2},
    {"ecirc", 0xEA}, {"icirc", 0xEE}, {"ocirc", 0xF4},
    {"ucirc", 0xFB}, {"atilde", 0xE3},{"otilde", 0xF5},
};

static long entity_lookup(const char *name, size_t n)
{
    for (size_t i = 0; i < sizeof(ENTITY_TABLE) / sizeof(ENTITY_TABLE[0]); i++)
    {
        if (strlen(ENTITY_TABLE[i].name) == n &&
            memcmp(ENTITY_TABLE[i].name, name, n) == 0)
            return (long)ENTITY_TABLE[i].ch;
    }
    return -1;
}

/* Encode the Unicode scalar cp as UTF-8 into out[0..3]. Returns the byte
 * count (1-4), or 0 for a non-scalar value (surrogate, > U+10FFFF). */
static size_t utf8_encode_cp(unsigned int cp, char out[4])
{
    if (cp <= 0x7F)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0; /* surrogate */
    if (cp <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF)
    {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Windows-1252 byte to Unicode code point. The 0x80-0x9F range differs
 * from latin-1 (curly quotes, dashes, euro sign...); undefined slots pass
 * through as their byte value. Web pages that are not UTF-8 are almost
 * always CP1252, so this is what the transcode below assumes. */
static unsigned int cp1252_cp(unsigned char b)
{
    static const unsigned short map[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (b < 0x80) return b;
    if (b < 0xA0) return map[b - 0x80];
    return b; /* latin-1 range */
}

/* Strict UTF-8 validation of s[0..n): rejects overlong encodings,
 * surrogates, values above U+10FFFF, and truncated sequences. */
static int utf8_valid(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        unsigned char first_lo, first_hi;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { need = 1; first_lo = 0x80; first_hi = 0xBF; }
        else if (c == 0xE0)         { need = 2; first_lo = 0xA0; first_hi = 0xBF; }
        else if (c >= 0xE1 && c <= 0xEC) { need = 2; first_lo = 0x80; first_hi = 0xBF; }
        else if (c == 0xED)         { need = 2; first_lo = 0x80; first_hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { need = 2; first_lo = 0x80; first_hi = 0xBF; }
        else if (c == 0xF0)         { need = 3; first_lo = 0x90; first_hi = 0xBF; }
        else if (c >= 0xF1 && c <= 0xF3) { need = 3; first_lo = 0x80; first_hi = 0xBF; }
        else if (c == 0xF4)         { need = 3; first_lo = 0x80; first_hi = 0x8F; }
        else return 0; /* 0x80-0xC1, 0xF5-0xFF: never a lead byte */
        if (i + need >= n) return 0; /* sequence truncated */
        for (size_t k = 1; k <= need; k++)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if (cc < 0x80 || cc > 0xBF) return 0;
        }
        unsigned char fc = (unsigned char)s[i + 1];
        if (fc < first_lo || fc > first_hi) return 0; /* overlong/surrogate */
        i += need + 1;
    }
    return 1;
}

/* Decode the entity starting at s[0] ('&' expected). On success writes
 * its UTF-8 bytes into out[0..3], sets *out_len (1-4) and *consumed (input
 * bytes advanced), and returns 1. Returns 0 when s[0] is not the start of
 * a recognized entity (caller emits '&' literally). Named-entity table
 * values are CP1252 bytes, so they map through cp1252_cp; numeric entities
 * keep their full code point (no 8-bit truncation). */
static int decode_entity(const char *s, size_t avail, char out[4],
                         size_t *out_len, size_t *consumed)
{
    if (avail < 2 || s[0] != '&') return 0;

    if (s[1] == '#')
    {
        size_t j = 2;
        int is_hex = 0;
        if (j < avail && (s[j] == 'x' || s[j] == 'X'))
        {
            is_hex = 1;
            j++;
        }
        unsigned long base = is_hex ? 16ul : 10ul;
        unsigned long v = 0;
        size_t digits = 0;
        while (j < avail && digits < 8)
        {
            unsigned char c = (unsigned char)s[j];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (is_hex && c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (is_hex && c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else break;
            if (v > (0x10FFFFul - d) / base) return 0; /* overflow / too large */
            v = v * base + d;
            digits++;
            j++;
        }
        if (digits == 0) return 0;
        if (j < avail && s[j] == ';') j++;
        size_t elen = utf8_encode_cp((unsigned)v, out);
        if (elen == 0) return 0; /* surrogate or out of range: not an entity */
        *out_len = elen;
        *consumed = j;
        return 1;
    }

    size_t j = 1;
    while (j < avail && j < 12 && isalnum((unsigned char)s[j])) j++;
    if (j == 1) return 0;
    long ch = entity_lookup(s + 1, j - 1);
    if (ch < 0) return 0;
    if (j < avail && s[j] == ';') j++;
    *out_len = utf8_encode_cp(cp1252_cp((unsigned char)ch), out);
    *consumed = j;
    return 1;
}

static size_t count_words(const char *s, size_t n)
{
    size_t words = 0;
    int in_word = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (isspace((unsigned char)s[i])) in_word = 0;
        else if (!in_word)
        {
            in_word = 1;
            words++;
        }
    }
    return words;
}

static int run_has_nonws(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (!isspace((unsigned char)s[i])) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Frames: per-block scoring (Crawl4AI PruningContentFilter port)      */
/* ------------------------------------------------------------------ */

typedef struct {
    TagId tag_id;       /* TAG_UNKNOWN for root */
    size_t start_pos;   /* raw offset just past the open tag */
    size_t end_pos;     /* raw offset of the matching close tag '<' */
    size_t text_len;    /* decoded text chars under this frame */
    size_t link_text_len;
    size_t word_count;
    size_t out_start;   /* output buffer offset when the frame opened */
    int link_at_open;   /* x->link_count when the frame opened */
    int class_neg;      /* class/id matched a negative pattern */
    int is_pre;
    int is_header;
} Frame;

static double frame_score(const Frame *f)
{
    size_t tag_len = f->end_pos - f->start_pos;
    double density = tag_len > 0 ? (double)f->text_len / (double)tag_len : 0.0;
    double link_density = f->text_len > 0
        ? 1.0 - (double)f->link_text_len / (double)f->text_len
        : 0.0;
    if (link_density < 0.0) link_density = 0.0;
    /* Crawl4AI computes a -0.5 class penalty but then clamps it to 0 via
     * max(0, ...), which makes the metric dead code; we keep the penalty so
     * nav/footer/sidebar-classed frames fall below the threshold. */
    double class_score = f->class_neg ? -0.5 : 0.0;
    double text_length = log((double)f->text_len + 1.0);
    /* metric weights: text_density 0.4, link_density 0.2, tag_weight 0.2,
     * class_id 0.1, text_length 0.1 (sum = 1.0) */
    return 0.4 * density + 0.2 * link_density + 0.2 * tag_weight_of(f->tag_id)
         + 0.1 * class_score + 0.1 * text_length;
}

/* ------------------------------------------------------------------ */
/* Extract context                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char *url;
} LinkRef;

typedef struct {
    const char *raw;
    size_t len;
    size_t max_chars;
    Writer w;
    Frame frames[MAX_FRAME_DEPTH];
    int depth;
    int skip_depth;   /* inside a hard-excluded tag */
    /* raw-text element (script/style) being skipped; its content may hold
     * '<' that must not be parsed as tags, so we scan for its close tag */
    char raw_tag[32];
    size_t raw_tag_len;
    OutBuf title;
    int title_active;
    char link_url[MAX_HREF_LEN];
    size_t link_url_len;
    int link_active;
    int link_has_text;
    LinkRef links[MAX_LINKS];
    int link_count;
} Extract;

/* Append one decoded byte to the title with whitespace collapsing. */
static int title_append_byte(OutBuf *b, unsigned char c, int *ws_pending)
{
    if (isspace(c))
    {
        *ws_pending = 1;
        return 0;
    }
    if (*ws_pending && b->len > 0)
    {
        if (outbuf_append_chr(b, ' ') != 0) return -1;
    }
    *ws_pending = 0;
    return outbuf_append_chr(b, (char)c);
}

/* Render a title text run: decode entities and transcode non-UTF-8 bytes
 * (see emit_text_run for why), collapsing whitespace. */
static int append_title_text(OutBuf *b, const char *s, size_t n)
{
    size_t i = 0;
    int ws_pending = 0;
    while (i < n)
    {
        size_t run_start = i;
        while (i < n && s[i] != '&') i++;
        size_t run_len = i - run_start;
        if (run_len > 0)
        {
            const char *run = s + run_start;
            if (utf8_valid(run, run_len))
            {
                for (size_t j = 0; j < run_len; j++)
                {
                    if (title_append_byte(b, (unsigned char)run[j],
                                          &ws_pending) != 0)
                        return -1;
                }
            }
            else
            {
                char buf[4];
                for (size_t j = 0; j < run_len; j++)
                {
                    unsigned char c = (unsigned char)run[j];
                    if (c < 0x80)
                    {
                        if (title_append_byte(b, c, &ws_pending) != 0)
                            return -1;
                    }
                    else
                    {
                        size_t blen = utf8_encode_cp(cp1252_cp(c), buf);
                        if (blen == 0)
                        {
                            buf[0] = (char)0xEF; buf[1] = (char)0xBF;
                            buf[2] = (char)0xBD; blen = 3; /* U+FFFD */
                        }
                        for (size_t k = 0; k < blen; k++)
                        {
                            if (title_append_byte(b, (unsigned char)buf[k],
                                                  &ws_pending) != 0)
                                return -1;
                        }
                    }
                }
            }
        }
        if (i >= n) break;
        char ebuf[4];
        size_t elen = 0, consumed = 0;
        if (decode_entity(s + i, n - i, ebuf, &elen, &consumed))
        {
            for (size_t k = 0; k < elen; k++)
            {
                if (title_append_byte(b, (unsigned char)ebuf[k],
                                      &ws_pending) != 0)
                    return -1;
            }
            i += consumed;
            continue;
        }
        /* not an entity: emit the '&' literally */
        if (title_append_byte(b, (unsigned char)'&', &ws_pending) != 0)
            return -1;
        i++;
    }
    return 0;
}

/* Appends a text run, sanitizing bytes that are not valid UTF-8: runs
 * that validate pass through unchanged; otherwise each byte is transcoded
 * Windows-1252 -> UTF-8 (the dominant legacy web encoding). Raw latin-1
 * high bytes in a tool result used to reach the frontend WebSocket
 * verbatim, and browsers drop a connection on the first text frame that
 * is not decodable as UTF-8 ("Could not decode a text frame as UTF-8");
 * strict LLM providers reject the same bytes in tool messages. */
static int emit_text_run(Writer *w, const char *s, size_t n, int is_pre)
{
    if (n == 0) return 0;
    if (utf8_valid(s, n))
    {
        if (is_pre)
        {
            if (outbuf_append(&w->out, s, n) != 0) return -1;
            w->has_last = 1;
            w->last = s[n - 1];
        }
        else
        {
            if (writer_text(w, s, n) != 0) return -1;
        }
        return 0;
    }
    char buf[4];
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        size_t blen;
        if (c < 0x80)
        {
            buf[0] = (char)c;
            blen = 1;
        }
        else
        {
            blen = utf8_encode_cp(cp1252_cp(c), buf);
        }
        if (blen == 0)
        {
            buf[0] = (char)0xEF; buf[1] = (char)0xBF;
            buf[2] = (char)0xBD; blen = 3; /* U+FFFD: undecodable byte */
        }
        if (is_pre)
        {
            if (outbuf_append(&w->out, buf, blen) != 0) return -1;
            w->has_last = 1;
            w->last = buf[blen - 1];
        }
        else
        {
            if (writer_text(w, buf, blen) != 0) return -1;
        }
    }
    return 0;
}

/* Render a text run: decode entities, collapse whitespace (unless inside
 * <pre>), count words/link-text for the enclosing frame. */
static int emit_text(Extract *x, const char *s, size_t n)
{
    if (n == 0) return 0;
    Frame *top = &x->frames[x->depth - 1];

    if (x->title_active)
        return append_title_text(&x->title, s, n);

    top->word_count += count_words(s, n);
    int is_pre = top->is_pre;
    if (is_pre && writer_preflush(&x->w) != 0) return -1;

    size_t i = 0;
    while (i < n)
    {
        size_t run_start = i;
        while (i < n && s[i] != '&' && s[i] != '<') i++;
        size_t run_len = i - run_start;
        if (run_len > 0)
        {
            if (emit_text_run(&x->w, s + run_start, run_len, is_pre) != 0)
                return -1;
            top->text_len += run_len;
            if (x->link_active)
            {
                top->link_text_len += run_len;
                if (run_has_nonws(s + run_start, run_len)) x->link_has_text = 1;
            }
        }
        if (i >= n) break;
        if (s[i] == '&')
        {
            char ebuf[4];
            size_t elen = 0, consumed = 0;
            if (decode_entity(s + i, n - i, ebuf, &elen, &consumed))
            {
                if (emit_text_run(&x->w, ebuf, elen, is_pre) != 0) return -1;
                top->text_len += 1;
                if (x->link_active)
                {
                    top->link_text_len += 1;
                    int all_ws = 1;
                    for (size_t k = 0; k < elen; k++)
                    {
                        if (!isspace((unsigned char)ebuf[k])) { all_ws = 0; break; }
                    }
                    if (!all_ws) x->link_has_text = 1;
                }
                i += consumed;
                continue;
            }
            /* not an entity: emit the '&' literally */
            if (emit_text_run(&x->w, "&", 1, is_pre) != 0) return -1;
            top->text_len += 1;
            if (x->link_active)
            {
                top->link_text_len += 1;
                x->link_has_text = 1;
            }
            i++;
            continue;
        }
        i++; /* stray '<' inside a text run: keep scanning */
    }
    return 0;
}

/* Close the top frame: accumulate metrics into the parent, then decide
 * keep/drop; dropped frames roll back their output (truncate-on-drop). */
static int frame_close(Extract *x, size_t pos)
{
    if (x->depth <= 1) return 0; /* root never closes */
    Frame *f = &x->frames[x->depth - 1];
    f->end_pos = pos;
    Frame *parent = &x->frames[x->depth - 2];
    parent->text_len += f->text_len;
    parent->link_text_len += f->link_text_len;
    parent->word_count += f->word_count;
    x->depth--;

    size_t min_words = f->is_header ? 0 : MIN_CONTENT_WORDS;
    if (frame_score(f) < SCORE_THRESHOLD || f->word_count < min_words)
    {
        outbuf_truncate(&x->w.out, f->out_start);
        writer_sync_state(&x->w);
        /* Links collected inside a dropped frame must not appear in the
         * footer (their text was rolled back, so citations would dangle). */
        for (int i = f->link_at_open; i < x->link_count; i++)
            free(x->links[i].url);
        x->link_count = f->link_at_open;
    }
    return 0;
}

static int link_resolve_index(Extract *x, const char *url, size_t len)
{
    if (len == 0) return -1;
    if (url[0] == '#') return -1;
    if (len >= 11 && memcmp(url, "javascript:", 11) == 0) return -1;
    for (int i = 0; i < x->link_count; i++)
    {
        if (strlen(x->links[i].url) == len &&
            memcmp(x->links[i].url, url, len) == 0)
            return i;
    }
    if (x->link_count >= MAX_LINKS) return -1;
    char *u = malloc(len + 1);
    if (!u) return -1;
    memcpy(u, url, len);
    u[len] = '\0';
    x->links[x->link_count].url = u;
    int idx = x->link_count;
    x->link_count++;
    return idx;
}

/* ------------------------------------------------------------------ */
/* Tag parsing helpers                                                 */
/* ------------------------------------------------------------------ */

static size_t find_seq(const char *s, size_t len, size_t start,
                       const char *pat, size_t plen)
{
    if (plen > len || start > len) return len;
    for (size_t i = start; i + plen <= len; i++)
    {
        size_t j = 0;
        while (j < plen && s[i + j] == pat[j]) j++;
        if (j == plen) return i;
    }
    return len;
}

/* Index of the '>' that terminates the tag starting at `start`, honoring
 * quoted attribute values (so '>' inside an attribute is not a terminator). */
static size_t find_tag_end(const char *s, size_t len, size_t start)
{
    char quote = 0;
    for (size_t i = start; i < len; i++)
    {
        char c = s[i];
        if (quote)
        {
            if (c == quote) quote = 0;
        }
        else if (c == '"' || c == '\'')
        {
            quote = c;
        }
        else if (c == '>')
        {
            return i;
        }
    }
    return len;
}

static void copy_attr_val(char *dst, size_t cap, size_t *dlen,
                          const char *val, size_t vlen)
{
    if (vlen >= cap) vlen = cap - 1;
    memcpy(dst, val, vlen);
    dst[vlen] = '\0';
    *dlen = vlen;
}

static int ieq(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++)
    {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* Scan the attribute list of an open tag (text after the tag name) and
 * extract href / class / id into caller buffers. Buffers are optional
 * (NULL skips that attribute). */
static void scan_attrs(const char *s, size_t n,
                       char *href, size_t href_cap, size_t *href_len,
                       char *cls, size_t cls_cap, size_t *cls_len,
                       char *idv, size_t idv_cap, size_t *idv_len)
{
    size_t i = 0;
    while (i < n && !isspace((unsigned char)s[i]) && s[i] != '/') i++;
    while (i < n)
    {
        while (i < n && (isspace((unsigned char)s[i]) || s[i] == '/')) i++;
        if (i >= n) break;
        size_t aname = i;
        while (i < n && !isspace((unsigned char)s[i]) &&
               s[i] != '=' && s[i] != '/')
            i++;
        size_t aname_len = i - aname;
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i < n && s[i] == '=')
        {
            i++;
            while (i < n && isspace((unsigned char)s[i])) i++;
            char quote = 0;
            if (i < n && (s[i] == '"' || s[i] == '\''))
            {
                quote = s[i];
                i++;
            }
            size_t vstart = i;
            if (quote)
            {
                while (i < n && s[i] != quote) i++;
            }
            else
            {
                while (i < n && !isspace((unsigned char)s[i]) && s[i] != '/') i++;
            }
            size_t vlen = i - vstart;
            const char *val = s + vstart;
            if (href && ieq(s + aname, aname_len, "href"))
                copy_attr_val(href, href_cap, href_len, val, vlen);
            else if (cls && ieq(s + aname, aname_len, "class"))
                copy_attr_val(cls, cls_cap, cls_len, val, vlen);
            else if (idv && ieq(s + aname, aname_len, "id"))
                copy_attr_val(idv, idv_cap, idv_len, val, vlen);
            if (quote && i < n && s[i] == quote) i++;
        }
        /* attribute without a value: nothing to extract */
    }
}

static int handle_open_tag(Extract *x, size_t tag_start, size_t tag_end)
{
    const char *s = x->raw + tag_start + 1;
    size_t n = tag_end - tag_start - 1;
    size_t name_len = 0;
    while (name_len < n && !isspace((unsigned char)s[name_len]) &&
           s[name_len] != '/' && s[name_len] != '>')
        name_len++;
    TagId tid = tag_lookup(s, name_len);
    int self_closing = n > 0 && s[n - 1] == '/';

    if (is_excluded_tag(tid))
    {
        x->skip_depth++;
        if (tid == TAG_EXCL_SCRIPT || tid == TAG_EXCL_STYLE)
        {
            /* raw-text: remember the element so the parse loop can skip to
             * its close tag without misparsing '<' inside its content */
            if (name_len < sizeof(x->raw_tag))
            {
                memcpy(x->raw_tag, s, name_len);
                x->raw_tag[name_len] = '\0';
                x->raw_tag_len = name_len;
            }
        }
        return 0;
    }
    if (tid == TAG_SPECIAL_TITLE)
    {
        if (x->skip_depth == 0) x->title_active = 1;
        return 0;
    }
    if (x->skip_depth > 0) return 0; /* inside excluded content: ignore */

    if (tid == TAG_VOID_NL_BR || tid == TAG_VOID_NL_HR)
    {
        writer_newline(&x->w);
        return 0;
    }
    if (tid == TAG_SPECIAL_A)
    {
        if (self_closing) return 0;
        x->link_url_len = 0;
        scan_attrs(s, n, x->link_url, MAX_HREF_LEN, &x->link_url_len,
                   NULL, 0, NULL, NULL, 0, NULL);
        x->link_active = 1;
        x->link_has_text = 0;
        return 0;
    }
    if (is_frame_tag(tid))
    {
        if (self_closing) return 0;
        if (x->depth >= MAX_FRAME_DEPTH) return 0; /* too deep: transparent */
        char cls[MAX_ATTR_LEN] = {0};
        size_t clen = 0;
        char idv[MAX_ATTR_LEN] = {0};
        size_t ilen = 0;
        scan_attrs(s, n, NULL, 0, NULL, cls, MAX_ATTR_LEN, &clen,
                   idv, MAX_ATTR_LEN, &ilen);
        Frame *f = &x->frames[x->depth];
        memset(f, 0, sizeof(*f));
        f->tag_id = tid;
        f->start_pos = tag_end + 1;
        f->out_start = x->w.out.len;
        f->link_at_open = x->link_count;
        f->is_pre = (tid == TAG_FRAME_PRE);
        f->is_header = is_header_tag(tid);
        f->class_neg = has_neg_pattern(cls, clen) || has_neg_pattern(idv, ilen);
        x->depth++;
        writer_newline(&x->w);
        return 0;
    }
    return 0; /* transparent (inline/unknown tags) */
}

static int handle_close_tag(Extract *x, size_t tag_start, size_t tag_end)
{
    const char *s = x->raw + tag_start + 2;
    size_t n = tag_end - tag_start - 2;
    size_t name_len = 0;
    while (name_len < n && !isspace((unsigned char)s[name_len]) &&
           s[name_len] != '>')
        name_len++;
    TagId tid = tag_lookup(s, name_len);

    if (tid == TAG_SPECIAL_A)
    {
        if (!x->link_active) return 0;
        x->link_active = 0;
        if (!x->link_has_text) return 0;
        int idx = link_resolve_index(x, x->link_url, x->link_url_len);
        if (idx < 0) return 0;
        char mk[16];
        int mlen = snprintf(mk, sizeof(mk), " [%d]", idx + 1);
        if (mlen <= 0 || (size_t)mlen >= sizeof(mk)) return 0;
        return writer_text(&x->w, mk, (size_t)mlen);
    }
    if (tid == TAG_SPECIAL_TITLE)
    {
        x->title_active = 0;
        return 0;
    }
    if (is_excluded_tag(tid))
    {
        if (x->skip_depth > 0) x->skip_depth--;
        return 0;
    }
    if (is_frame_tag(tid))
    {
        /* find the matching open frame, auto-closing misnested ones above */
        int d = x->depth - 1;
        while (d > 0 && x->frames[d].tag_id != tid) d--;
        if (d <= 0) return 0;
        while (x->depth - 1 > d)
        {
            if (frame_close(x, tag_start) != 0) return -1;
        }
        return frame_close(x, tag_start);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main parse loop                                                     */
/* ------------------------------------------------------------------ */

static int parse(Extract *x)
{
    size_t i = 0;
    while (i < x->len)
    {
        if (x->raw[i] == '<' && x->skip_depth > 0 && x->raw_tag_len > 0)
        {
            /* raw-text skip (script/style): jump to the literal close tag;
             * content may contain '<' that would confuse the tag scanner */
            size_t k = x->len;
            for (size_t j = i; j + 2 + x->raw_tag_len <= x->len; j++)
            {
                if (x->raw[j] == '<' && x->raw[j + 1] == '/' &&
                    memcmp(x->raw + j + 2, x->raw_tag, x->raw_tag_len) == 0)
                {
                    k = j;
                    break;
                }
            }
            if (k == x->len)
            {
                i = x->len;
                x->skip_depth = 0;
                x->raw_tag_len = 0;
                continue;
            }
            size_t te = find_tag_end(x->raw, x->len, k + 1);
            i = (te == x->len) ? x->len : te + 1;
            x->skip_depth--;
            x->raw_tag_len = 0;
            continue;
        }
        if (x->raw[i] != '<')
        {
            size_t j = i;
            while (j < x->len && x->raw[j] != '<') j++;
            if (x->skip_depth == 0 && emit_text(x, x->raw + i, j - i) != 0)
                return -1;
            i = j;
            continue;
        }

        if (i + 1 < x->len && x->raw[i + 1] == '!')
        {
            size_t k;
            if (i + 3 < x->len && x->raw[i + 2] == '-' && x->raw[i + 3] == '-')
            {
                /* <!-- comment --> */
                k = find_seq(x->raw, x->len, i + 4, "-->", 3);
                i = (k != x->len) ? k + 3 : x->len;
            }
            else if (i + 8 < x->len &&
                     strncmp(x->raw + i + 2, "[CDATA[", 7) == 0)
            {
                k = find_seq(x->raw, x->len, i + 9, "]]>", 3);
                i = (k != x->len) ? k + 3 : x->len;
            }
            else
            {
                /* doctype or other declaration */
                k = find_seq(x->raw, x->len, i + 2, ">", 1);
                i = (k != x->len) ? k + 1 : x->len;
            }
            continue;
        }

        if (i + 1 < x->len && x->raw[i + 1] == '?')
        {
            size_t k = find_seq(x->raw, x->len, i + 2, ">", 1);
            i = (k != x->len) ? k + 1 : x->len;
            continue;
        }

        size_t te = find_tag_end(x->raw, x->len, i + 1);
        if (te == x->len)
        {
            /* stray '<' with no tag terminator: treat as text */
            if (x->skip_depth == 0 && emit_text(x, x->raw + i, 1) != 0)
                return -1;
            i++;
            continue;
        }

        int rc = (te > i + 1 && x->raw[i + 1] == '/')
            ? handle_close_tag(x, i, te)
            : handle_open_tag(x, i, te);
        if (rc != 0) return -1;
        i = te + 1;
    }

    while (x->depth > 1)
    {
        if (frame_close(x, x->len) != 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Final assembly: title + body + truncation marker + links footer     */
/* ------------------------------------------------------------------ */

static int build_footer(Extract *x, OutBuf *fb)
{
    if (x->link_count == 0) return 0;
    if (outbuf_append(fb, "\n\nLinks:\n", 9) != 0) return -1;
    for (int i = 0; i < x->link_count; i++)
    {
        char num[16];
        int nl = snprintf(num, sizeof(num), "%d. ", i + 1);
        if (nl <= 0 || (size_t)nl >= sizeof(num)) continue;
        if (outbuf_append(fb, num, (size_t)nl) != 0) return -1;
        if (outbuf_append(fb, x->links[i].url, strlen(x->links[i].url)) != 0)
            return -1;
        if (outbuf_append_chr(fb, '\n') != 0) return -1;
    }
    return 0;
}

/* Copies title-prefix, body, marker, and footer into dst in canonical
 * order, returning the number of bytes written (excluding the NUL). */
static size_t assemble_copy_parts(char *dst, const char *title, size_t tlen,
                                  const char *body, size_t body_len,
                                  const char *footer, size_t flen,
                                  const char *marker, size_t mlen)
{
    size_t w = 0;
    if (title)
    {
        const char *tprefix = "Title: ";
        size_t plen = 7;
        memcpy(dst + w, tprefix, plen); w += plen;
        memcpy(dst + w, title, tlen); w += tlen;
        memcpy(dst + w, "\n\n", 2);   w += 2;
    }
    if (body_len > 0)
    {
        memcpy(dst + w, body, body_len);
        w += body_len;
    }
    if (mlen > 0) { memcpy(dst + w, marker, mlen); w += mlen; }
    if (flen > 0) { memcpy(dst + w, footer, flen); w += flen; }
    return w;
}

/* Backs the cut point off so the byte at data[cut] is not a UTF-8
 * continuation byte: cutting there would split a multi-byte character and
 * leave the output invalid UTF-8 (which corrupts WebSocket text frames). */
static size_t utf8_cut_boundary(const char *data, size_t cut)
{
    while (cut > 0 && ((unsigned char)data[cut] & 0xC0) == 0x80)
        cut--;
    return cut;
}

/* Computes how much of the body fits the truncated budget: prefer a
 * paragraph boundary, then a word boundary, over a mid-word cut. Sets
 * *omitted to the dropped byte count. */
static size_t assemble_cut(const OutBuf *out, size_t budget, size_t *omitted)
{
    size_t body_len = out->len;
    /* Case 2 guarantees body_len > budget (else total would have fit), but
     * keep the guard so the boundary walk can never shrink a body that
     * already fits the budget. */
    if (body_len <= budget)
    {
        *omitted = 0;
        return body_len;
    }
    size_t cut = budget;
    size_t k = cut;
    while (k > 0 && out->data[k - 1] != '\n') k--;
    if (k > 0 && k != cut)
    {
        cut = k; /* paragraph boundary */
    }
    else if (k == 0)
    {
        k = cut;
        while (k > 0 && out->data[k - 1] != ' ') k--;
        if (k > 0) cut = k; /* word boundary fallback */
    }
    cut = utf8_cut_boundary(out->data, cut);
    *omitted = body_len - cut;
    return cut;
}

static char *assemble(Extract *x)
{
    size_t max_chars = x->max_chars;
    char *title = x->title.len > 0 ? x->title.data : NULL;
    size_t tlen = title ? x->title.len + 7 : 0; /* "Title: " prefix + text */
    size_t body_len = x->w.out.len;

    OutBuf footer = {0};
    if (build_footer(x, &footer) != 0) { free(footer.data); return NULL; }
    size_t flen = footer.len;

    size_t total = body_len + tlen + flen + (tlen > 0 ? 2 : 0);
    if (total <= max_chars)
    {
        char *res = malloc(total + 1);
        if (!res) { free(footer.data); return NULL; }
        size_t w = assemble_copy_parts(res, title, x->title.len,
                                       x->w.out.data, body_len,
                                       footer.data, flen, NULL, 0);
        res[w] = '\0';
        free(footer.data);
        return res;
    }

    /* Truncate: drop the footer first, then the title, when the budget is
     * too tight to fit them alongside the truncated body. */
    size_t overhead = tlen + (tlen > 0 ? 2 : 0) + flen;
    if (overhead + MARKER_ROOM > max_chars)
    {
        free(footer.data);
        footer.data = NULL;
        footer.len = 0;
        flen = 0;
        overhead = tlen + (tlen > 0 ? 2 : 0);
    }
    if (overhead + MARKER_ROOM > max_chars)
    {
        title = NULL;
        tlen = 0;
        overhead = 0;
    }

    size_t budget = 0;
    int marker_ok = 0;
    if (max_chars > overhead + MARKER_ROOM)
    {
        budget = max_chars - overhead - MARKER_ROOM;
        marker_ok = 1;
    }
    else if (max_chars > overhead)
    {
        budget = max_chars - overhead;
    }

    size_t omitted = 0;
    size_t cut = assemble_cut(&x->w.out, budget, &omitted);
    if (body_len <= budget) marker_ok = 0;

    char marker[64];
    size_t mlen = 0;
    if (marker_ok && omitted > 0)
    {
        int ml = snprintf(marker, sizeof(marker),
                          "\n[... truncated, %zu chars omitted ...]", omitted);
        if (ml < 0) ml = 0;
        if ((size_t)ml >= sizeof(marker)) ml = (int)sizeof(marker) - 1;
        /* keep the marker only if it actually fits alongside the cut body */
        if (tlen + (tlen > 0 ? 2 : 0) + cut + (size_t)ml + flen <= max_chars)
            mlen = (size_t)ml;
    }

    size_t fin = tlen + (tlen > 0 ? 2 : 0) + cut + mlen + flen;
    char *res = malloc(fin + 1);
    if (!res) { free(footer.data); return NULL; }
    size_t w = assemble_copy_parts(res, title, x->title.len,
                                   x->w.out.data, cut,
                                   footer.data, flen, marker, mlen);
    res[w] = '\0';
    free(footer.data);
    return res;
}

char *html_extract_text_alloc(const char *raw, size_t raw_len, size_t max_chars)
{
    if (!raw) return str_dup("");
    if (max_chars == 0) return str_dup("");

    Extract x;
    memset(&x, 0, sizeof(x));
    x.raw = raw;
    x.len = raw_len;
    x.max_chars = max_chars;
    x.frames[0].tag_id = -1; /* root frame: never scored, never dropped */
    x.depth = 1;

    if (parse(&x) != 0)
    {
        free(x.w.out.data);
        free(x.title.data);
        for (int i = 0; i < x.link_count; i++) free(x.links[i].url);
        return NULL;
    }

    char *res = assemble(&x);
    free(x.w.out.data);
    free(x.title.data);
    for (int i = 0; i < x.link_count; i++) free(x.links[i].url);
    return res;
}

/* ------------------------------------------------------------------ */
/* Content-type dispatch                                               */
/* ------------------------------------------------------------------ */

static int prefix_ieq(const char *s, const char *pat)
{
    size_t plen = strlen(pat);
    for (size_t i = 0; i < plen; i++)
    {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pat[i]))
            return 0;
    }
    return 1;
}

/* Wrap a non-NUL-terminated byte range for the NUL-terminated helpers. */
static char *text_for_llm(const char *data, size_t len, size_t max_chars)
{
    char *nul = malloc(len + 1);
    if (!nul) return NULL;
    memcpy(nul, data, len);
    nul[len] = '\0';
    if (!utf8_valid(nul, len))
    {
        /* Plain-text responses need not be UTF-8 (latin-1/CP1252 is
         * common); transcode so the result is always decodable — invalid
         * bytes in a tool result break the frontend WebSocket and strict
         * LLM providers. Transcoded output is at most 2x, so allocate for
         * the worst case and copy back. */
        char *fixed = NULL;
        if (len <= SIZE_MAX / 2)
            fixed = malloc(len * 2 + 1);
        if (fixed)
        {
            size_t w = 0;
            char buf[4];
            for (size_t i = 0; i < len; i++)
            {
                unsigned char c = (unsigned char)nul[i];
                size_t blen;
                if (c < 0x80)
                {
                    buf[0] = (char)c;
                    blen = 1;
                }
                else
                {
                    blen = utf8_encode_cp(cp1252_cp(c), buf);
                }
                if (blen == 0)
                {
                    buf[0] = (char)0xEF; buf[1] = (char)0xBF;
                    buf[2] = (char)0xBD; blen = 3; /* U+FFFD */
                }
                memcpy(fixed + w, buf, blen);
                w += blen;
            }
            fixed[w] = '\0';
            free(nul);
            nul = fixed;
            len = w;
        }
    }
    char *res = str_truncate_ellipsis_dup(nul, max_chars);
    free(nul);
    return res;
}

char *content_extract_for_llm_alloc(const char *content_type, const char *data,
                              size_t len, size_t max_chars)
{
    if (!data || len == 0) return str_dup("(empty response)");
    if (max_chars == 0) return str_dup("");

    if (!content_type || !content_type[0])
    {
        /* no Content-Type header: sniff the first non-space byte */
        size_t i = 0;
        while (i < len && isspace((unsigned char)data[i])) i++;
        if (i < len && data[i] == '<') return html_extract_text_alloc(data, len, max_chars);
        return text_for_llm(data, len, max_chars);
    }

    if (prefix_ieq(content_type, "text/html") ||
        prefix_ieq(content_type, "application/xhtml+xml"))
        return html_extract_text_alloc(data, len, max_chars);

    if (prefix_ieq(content_type, "text/") ||
        prefix_ieq(content_type, "application/json") ||
        prefix_ieq(content_type, "application/xml") ||
        prefix_ieq(content_type, "application/javascript"))
        return text_for_llm(data, len, max_chars);

    /* binary: never dump raw bytes into the LLM context */
    size_t ctlen = strlen(content_type);
    if (ctlen > 512) ctlen = 512;
    size_t cap = 160 + ctlen + 32;
    char *res = malloc(cap);
    if (!res) return NULL;
    int n = snprintf(res, cap,
                     "Binary content (%.*s, %zu bytes). Not shown; "
                     "use a file tool to download it if needed.",
                     (int)ctlen, content_type, len);
    if (n < 0 || (size_t)n >= cap)
    {
        free(res);
        return str_dup("Binary content (unrecognized type).");
    }
    return res;
}
