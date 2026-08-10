/*
 * html_extract.h - readable-text extraction from raw HTML (Crawl4AI-style
 * pipeline) and Content-Type dispatch for fetched web bytes.
 * Depends on: stddef.h, string_utils.h (str_truncate_ellipsis_dup).
 */

#ifndef ECHO_HTML_EXTRACT_H
#define ECHO_HTML_EXTRACT_H

#include <stddef.h>

/**
 * html_extract_text_alloc - extract readable text from raw HTML bytes
 * @raw: HTML bytes; need not be NUL-terminated. NULL is accepted and
 *   yields the empty string.
 * @raw_len: authoritative byte length of raw.
 * @max_chars: output budget for the final string; 0 yields the empty
 *   string.
 *
 * Crawl4AI-style pipeline: strips script/style/nav/footer/header/aside/
 * form/iframe/noscript and other low-value blocks (pruned by text
 * density / link density / tag weight), decodes entities, collapses
 * whitespace, maps block tags to newlines, converts links to a citation
 * system ("text [1]" plus a "Links:" footer), and head-biased-truncates
 * at a paragraph boundary to fit max_chars.
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 * (free with free()), or NULL on allocation failure. No shared state;
 * safe to call concurrently.
 */
char *html_extract_text_alloc(const char *raw, size_t raw_len, size_t max_chars);

/**
 * content_extract_for_llm_alloc - dispatch raw fetched bytes by Content-Type
 * @content_type: HTTP Content-Type header value, or NULL to sniff.
 * @data: response bytes; need not be NUL-terminated (len is
 *   authoritative). NULL data or len == 0 yields "(empty response)".
 * @len: authoritative byte length of data.
 * @max_chars: output budget; 0 yields the empty string.
 *
 * Dispatch rules: text/html and application/xhtml+xml go through
 * html_extract_text_alloc; text/plain, application/json, application/xml and
 * application/javascript get an ellipsis-truncated passthrough; any
 * other type (binary: images, application/pdf, ...) yields a short
 * descriptor string instead of raw bytes. With a NULL content_type the
 * first non-space byte is sniffed: '<' takes the HTML path.
 *
 * Return: caller-owned malloc'd NUL-terminated string (free with
 * free()), or NULL on allocation failure. No shared state; safe to call
 * concurrently.
 */
char *content_extract_for_llm_alloc(const char *content_type, const char *data,
                              size_t len, size_t max_chars);

#ifdef HTML_EXTRACT_TEST
/**
 * html_extract_test_set_alloc_fail - arm the allocation-failure hook
 * @nth_allocation: 1-based index of the next html_extract_text_alloc /
 *   content_extract_for_llm_alloc internal allocation that should fail; -1
 *   disables fault injection.
 *
 * Test-only hook for allocation-failure regression tests (AGENTS.md
 * section 11). The call counter resets on every arm, so the index
 * counts from the next extractor call onward.
 *
 * Return: void; never fails.
 */
void html_extract_test_set_alloc_fail(int nth_allocation);
#endif

#endif
