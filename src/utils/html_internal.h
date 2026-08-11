/*
 * html_internal.h - shared state and cross-unit contracts for the
 * readable-text extraction pipeline, split across html_unicode/entities/
 * tags/outbuf/writer units. Documented exception to the one-header-per-
 * module rule: the pipeline passes one Extract context plus its embedded
 * buffer/writer/frame types through every stage, so the structs and
 * constants live here instead of being duplicated. Function contracts
 * live in the unit headers. Not installed or included outside src/utils.
 * Depends on: html_extract.h (public API types).
 */

#ifndef ECHO_HTML_INTERNAL_H
#define ECHO_HTML_INTERNAL_H

#include <stddef.h>

#include "html_extract.h"

#define MAX_FRAME_DEPTH 64
#define MAX_LINKS 20
#define MAX_HREF_LEN 2048
#define MAX_ATTR_LEN 256
#define SCORE_THRESHOLD 0.48
#define MIN_CONTENT_WORDS 4
/* Reserved room for the truncation marker; 46 covers a marker with up to
 * 33 omitted-digit counts, which far exceeds any realistic page size. */
#define MARKER_ROOM 46

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

/* Crawl4AI negative_patterns: class/id containing any of these is
 * boilerplate (defined in html_tags.c, used there and in the core). */
extern const char *const NEG_PATTERNS[];
#define NEG_PATTERNS_COUNT 10U

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} OutBuf;

typedef struct {
    OutBuf out;
    int need_newline; /* block boundary requested but not yet emitted */
    int ws_pending;   /* collapsed space seen, not yet emitted */
    int has_last;
    char last;
} Writer;

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

#ifdef HTML_EXTRACT_TEST
/* Allocation shims defined non-static in html_extract.c under the same
 * guard; every html TU compiled with HTML_EXTRACT_TEST routes its
 * malloc/realloc through them (via per-TU #defines after includes) so
 * html_extract_test_set_alloc_fail() reaches the whole pipeline. */
void *test_realloc(void *ptr, size_t size);
void *test_malloc(size_t size);
#endif

#endif /* ECHO_HTML_INTERNAL_H */
