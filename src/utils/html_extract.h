#ifndef ECHO_HTML_EXTRACT_H
#define ECHO_HTML_EXTRACT_H

#include <stddef.h>

/* Extract readable text from raw HTML bytes (Crawl4AI-style pipeline):
 *  - strips script/style/nav/footer/header/aside/form/iframe/noscript etc.
 *  - prunes low-value blocks by text density / link density / tag weight
 *  - decodes entities, collapses whitespace, block tags -> newlines
 *  - converts links to a citation system: "text [1]" + a "Links:" footer
 *  - head-biased truncation at a paragraph boundary, capped at `max_chars`
 *
 * The returned string is freshly malloc'd, NUL-terminated, owned by the
 * caller (free with `free`). Returns NULL only on allocation failure.
 * `raw` need not be NUL-terminated; `raw_len` is authoritative. */
char *html_extract_text(const char *raw, size_t raw_len, size_t max_chars);

/* Dispatch raw fetched bytes to the right simplification path based on the
 * HTTP Content-Type header (may be NULL):
 *  - text/html  -> html_extract_text
 *  - text/plain / application/json / other text types -> truncated passthrough
 *  - binary (image/..., application/pdf, ...) -> short descriptor string
 * Ownership and failure mode are identical to html_extract_text. */
char *content_extract_for_llm(const char *content_type, const char *data,
                              size_t len, size_t max_chars);

#ifdef HTML_EXTRACT_TEST
/* Fault injection: fail the nth internal allocation (1-based) of the next
 * html_extract_text / content_extract_for_llm call; -1 disables. */
void html_extract_test_set_alloc_fail(int nth_allocation);
#endif

#endif
