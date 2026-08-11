#include <check.h>

#include "utils/html_extract.h"
#include "utils/string_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test_html_extract - unit tests for html extract. Depends on: check, the module under test. */
static Suite *html_extract_suite(void);

int main(void)
{
    SRunner *sr = srunner_create(html_extract_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}

/* ---- basic extraction ---- */

START_TEST(test_extracts_plain_text)
{
    char *out = html_extract_text_alloc("<html><body><p>Hello world this is fine.</p></body></html>",
                                  strlen("<html><body><p>Hello world this is fine.</p></body></html>"),
                                  1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "Hello world this is fine.");
    free(out);
}
END_TEST

START_TEST(test_strips_boilerplate_tags)
{
    const char *html =
        "<html><head><title>Test Page</title></head><body>"
        "<nav>Home About</nav>"
        "<footer>Copyright 2026</footer>"
        "<aside>Related links</aside>"
        "<div>Main content here now.</div>"
        "</body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "Title: Test Page\n\nMain content here now.");
    free(out);
}
END_TEST

START_TEST(test_strips_script_and_style)
{
    const char *html =
        "<html><body>"
        "<script>var x = 1 < 2;</script>"
        "<style>p { color: red; }</style>"
        "<p>Real text content here.</p>"
        "</body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "Real text content here.");
    free(out);
}
END_TEST

START_TEST(test_decodes_entities)
{
    const char *html = "<p>Fish &amp; chips &#65;&#x42; &lt;tag&gt; &quot;quoted&quot;</p>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "Fish & chips AB <tag> \"quoted\"");
    free(out);
}
END_TEST

START_TEST(test_literal_ampersand_preserved)
{
    const char *html = "<p>a && b && c && d</p>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "a && b && c && d");
    free(out);
}
END_TEST

START_TEST(test_block_tags_become_newlines)
{
    const char *html = "<p>one two three four</p><p>five six seven eight</p>"
                       "<p>nine ten eleven twelve</p>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "one two three four\nfive six seven eight\nnine ten eleven twelve");
    free(out);
}
END_TEST

START_TEST(test_collapses_whitespace)
{
    const char *html = "<p>  lots   of\n\t spaces  here  </p>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "lots of spaces here");
    free(out);
}
END_TEST

START_TEST(test_inline_tags_do_not_break_words)
{
    const char *html = "<p>a very <b>bold</b> word now</p>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "a very bold word now");
    free(out);
}
END_TEST

START_TEST(test_title_is_prefixed)
{
    const char *html = "<html><head><title>My Page</title></head><body><p>hi there everyone now</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "Title: My Page\n\nhi there everyone now");
    free(out);
}
END_TEST

START_TEST(test_citations_collected)
{
    const char *html =
        "<html><body>"
        "<p>See <a href=\"https://example.com/a\">this link</a> and "
        "<a href=\"https://example.com/b\">that one</a>.</p>"
        "</body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out,
                     "See this link [1] and that one [2].\n\n"
                     "Links:\n1. https://example.com/a\n2. https://example.com/b\n");
    free(out);
}
END_TEST

START_TEST(test_citation_deduplicated)
{
    const char *html =
        "<html><body><p><a href=\"https://example.com/a\">first</a> then "
        "<a href=\"https://example.com/a\">again</a>.</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out,
                     "first [1] then again [1].\n\nLinks:\n1. https://example.com/a\n");
    free(out);
}
END_TEST

START_TEST(test_nav_class_blocks_pruned)
{
    /* Link-heavy divs with nav/sidebar class or id: link_density 0 plus the
     * class penalty drops them below the 0.48 threshold; their links must
     * not leak into the citations footer either. */
    const char *html =
        "<html><body>"
        "<div class=\"navigation\"><a href=\"/\">Home</a>"
        "<a href=\"/about\">About</a>"
        "<a href=\"/products\">Products</a>"
        "<a href=\"/pricing\">Pricing</a></div>"
        "<div id=\"sidebar\"><a href=\"t\">tag one</a>"
        "<a href=\"u\">tag two</a></div>"
        "<article>Real article content here.</article>"
        "</body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "Real article content here.");
    free(out);
}
END_TEST

START_TEST(test_small_budget_truncates_with_marker)
{
    char html[800];
    size_t n = 0;
    n += (size_t)snprintf(html + n, sizeof(html) - n,
                          "<html><head><title>Big Page</title></head><body><p>");
    for (int i = 0; i < 100; i++)
        n += (size_t)snprintf(html + n, sizeof(html) - n, "word%d ", i);
    n += (size_t)snprintf(html + n, sizeof(html) - n, "</p></body></html>");
    ck_assert_uint_le(n, sizeof(html));

    char *out = html_extract_text_alloc(html, n, 120);
    ck_assert_ptr_nonnull(out);
    ck_assert_uint_le(strlen(out), 120);
    ck_assert_int_eq(strncmp(out, "Title: Big Page\n\n", 17), 0);
    ck_assert_str_eq(out + 17, "word0 word1 word2 word3 word4 word5 word6 word7 word8 "
                               "\n[... truncated, 635 chars omitted ...]");
    free(out);
}
END_TEST

/* ---- dispatch ---- */

START_TEST(test_dispatch_html_by_type)
{
    const char *html = "<html><body><p>typed html content here</p></body></html>";
    char *out = content_extract_for_llm_alloc("text/html; charset=utf-8",
                                        html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "typed html content here");
    free(out);
}
END_TEST

START_TEST(test_dispatch_sniffs_missing_type)
{
    const char *html = "<!DOCTYPE html><p>sniffed content works fine</p>";
    char *out = content_extract_for_llm_alloc(NULL, html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "sniffed content works fine");
    free(out);
}
END_TEST

START_TEST(test_dispatch_plain_text_passthrough)
{
    const char *txt = "just some plain text";
    char *out = content_extract_for_llm_alloc("text/plain", txt, strlen(txt), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "just some plain text");
    free(out);
}
END_TEST

START_TEST(test_dispatch_json_truncated)
{
    const char *json = "{\"key\":\"value\"}";
    char *out = content_extract_for_llm_alloc("application/json", json, strlen(json), 10);
    ck_assert_ptr_nonnull(out);
    ck_assert_uint_le(strlen(out), 10);
    free(out);
}
END_TEST

START_TEST(test_dispatch_binary_descriptor)
{
    const char *bin = "\x89PNG\r\n\x1a\n";
    char *out = content_extract_for_llm_alloc("image/png", bin, 8, 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out,
                     "Binary content (image/png, 8 bytes). "
                     "Not shown; use a file tool to download it if needed.");
    free(out);
}
END_TEST

/* ---- OOM fault injection ---- */

START_TEST(test_oom_allocation_failure)
{
    const char *html = "<html><body><p>alpha beta gamma delta</p>"
                       "<p>epsilon zeta eta theta</p></body></html>";
    size_t n = strlen(html);
    html_extract_test_set_alloc_fail(1);
    char *out = html_extract_text_alloc(html, n, 1000);
    ck_assert_ptr_null(out);
    html_extract_test_set_alloc_fail(-1);
    out = html_extract_text_alloc(html, n, 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "alpha beta gamma delta\nepsilon zeta eta theta");
    free(out);
}
END_TEST

START_TEST(test_oom_mid_parse_rolls_back)
{
    const char *html = "<html><body><p>gamma delta kappa lambda</p>"
                       "<p>mu nu xi omicron</p></body></html>";
    size_t n = strlen(html);
    /* Fail the final output allocation: must return NULL, not partial text. */
    html_extract_test_set_alloc_fail(2);
    char *out = html_extract_text_alloc(html, n, 1000);
    ck_assert_ptr_null(out);
    html_extract_test_set_alloc_fail(-1);
    out = html_extract_text_alloc(html, n, 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "gamma delta kappa lambda\nmu nu xi omicron");
    free(out);
}
END_TEST

/* ---- str_truncate_ellipsis_dup ---- */

START_TEST(test_ellipsis_short_passthrough)
{
    char *out = str_truncate_ellipsis_dup("short", 100);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "short");
    free(out);
}
END_TEST

START_TEST(test_ellipsis_truncates)
{
    char *out = str_truncate_ellipsis_dup("0123456789abcdef", 10);
    ck_assert_ptr_nonnull(out);
    ck_assert_uint_le(strlen(out), 10);
    ck_assert_str_eq(out, "0123456789");
    free(out);
}
END_TEST

START_TEST(test_ellipsis_keeps_marker_when_room)
{
    char *out = str_truncate_ellipsis_dup("0123456789abcdef0123456789abcdef0123456789abcdef", 45);
    ck_assert_ptr_nonnull(out);
    ck_assert_uint_le(strlen(out), 45);
    ck_assert_str_eq(out, "012345678[... truncated, 3 chars omitted ...]");
    free(out);
}
END_TEST

/* ---- UTF-8 output validity ----
 * Regression for the "Could not decode a text frame as UTF-8" WebSocket
 * deaths: entity decoding used to emit raw latin-1 bytes (0x80-0xFF) and
 * numeric entities truncated to 8 bits, so tool results carried invalid
 * UTF-8 into the tool_end/done frames and browsers killed the connection.
 * Every extraction result must be decodable as UTF-8. */

static int is_valid_utf8(const char *s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        unsigned char first_lo, first_hi;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xE0) {
            need = 2;
            first_lo = 0xA0;
            first_hi = 0xBF;
        }
        else if (c >= 0xE1 && c <= 0xEC) {
            need = 2;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xED) {
            need = 2;
            first_lo = 0x80;
            first_hi = 0x9F;
        }
        else if (c >= 0xEE && c <= 0xEF) {
            need = 2;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xF0) {
            need = 3;
            first_lo = 0x90;
            first_hi = 0xBF;
        }
        else if (c >= 0xF1 && c <= 0xF3) {
            need = 3;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xF4) {
            need = 3;
            first_lo = 0x80;
            first_hi = 0x8F;
        }
        else return 0;
        if (i + need >= n) return 0;
        for (size_t k = 1; k <= need; k++)
        {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        }
        unsigned char fc = (unsigned char)s[i + 1];
        if (fc < first_lo || fc > first_hi) return 0;
        i += need + 1;
    }
    return 1;
}

START_TEST(test_entities_decode_to_valid_utf8)
{
    const char *html = "<html><body><p>Caf&eacute; au lait &mdash; "
                       "r&eacute;sum&eacute; fini</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out,
                     "Caf\xC3\xA9 au lait \xE2\x80\x94 r\xC3\xA9sum\xC3\xA9 fini");
    free(out);
}
END_TEST

START_TEST(test_numeric_entities_keep_full_codepoint)
{
    /* &#8212; (U+2014) was truncated to an 8-bit control byte before. */
    const char *html = "<html><body><p>Dash &#8212; here &amp; there now ok</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out, "Dash \xE2\x80\x94 here & there now ok");
    free(out);
}
END_TEST

START_TEST(test_latin1_body_transcoded)
{
    const char *html = "<html><body><p>caf\xE9 au lait chaud ici</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out, "caf\xC3\xA9 au lait chaud ici");
    free(out);
}
END_TEST

START_TEST(test_utf8_body_passthrough_unchanged)
{
    /* Valid UTF-8 input must not be touched by the transcode. */
    const char *html = "<html><body><p>caf\xC3\xA9 au lait chaud ici</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out, "caf\xC3\xA9 au lait chaud ici");
    free(out);
}
END_TEST

START_TEST(test_title_entities_valid_utf8)
{
    const char *html = "<html><head><title>Caf&eacute; menu</title></head>"
                       "<body><p>real body text now</p></body></html>";
    char *out = html_extract_text_alloc(html, strlen(html), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out,
                     "Title: Caf\xC3\xA9 menu\n\nreal body text now");
    free(out);
}
END_TEST

START_TEST(test_plain_text_latin1_sanitized)
{
    const char *txt = "caf\xE9 au lait chaud ici";
    char *out = content_extract_for_llm_alloc("text/plain", txt, strlen(txt), 1000);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out, "caf\xC3\xA9 au lait chaud ici");
    free(out);
}
END_TEST

START_TEST(test_truncation_does_not_split_utf8)
{
    /* A long run of multi-byte chars with no spaces: the byte budget lands
     * mid-sequence; the cut must back off to a character boundary. */
    char body[256];
    size_t n = 0;
    body[n++] = '<'; body[n++] = 'p'; body[n++] = '>';
    for (int i = 0; i < 40; i++)
    {
        body[n++] = (char)0xC3;
        body[n++] = (char)0xA9; /* e-acute as UTF-8 */
    }
    body[n++] = '<'; body[n++] = '/'; body[n++] = 'p'; body[n++] = '>';
    char *out = html_extract_text_alloc(body, n, 10);
    ck_assert_ptr_nonnull(out);
    ck_assert_uint_le(strlen(out), 10);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    free(out);
}
END_TEST

START_TEST(test_ellipsis_utf8_boundary)
{
    /* str_truncate_ellipsis_dup must not cut inside a multi-byte sequence:
     * the result feeds WebSocket frames. max 5 on "ab" + 10 x e-acute
     * (22 bytes) leaves no marker room; keep=5 lands between the third
     * e-acute's lead and continuation bytes, so it backs off to 4
     * ("ab" + one e-acute) instead of emitting a dangling lead byte. */
    char input[64];
    size_t n = 0;
    input[n++] = 'a'; input[n++] = 'b';
    for (int i = 0; i < 10; i++)
    {
        input[n++] = (char)0xC3;
        input[n++] = (char)0xA9;
    }
    input[n] = '\0';
    char *out = str_truncate_ellipsis_dup(input, 5);
    ck_assert_ptr_nonnull(out);
    ck_assert_uint_le(strlen(out), 5);
    ck_assert_int_eq(is_valid_utf8(out), 1);
    ck_assert_str_eq(out, "ab\xC3\xA9");
    free(out);
}
END_TEST

static Suite *html_extract_suite(void)
{
    /* E15: split the former single flat tcase into per-area TCases */
    Suite *s = suite_create("html_extract");

    TCase *tc = tcase_create("Extraction");
    tcase_add_test(tc, test_extracts_plain_text);
    tcase_add_test(tc, test_strips_boilerplate_tags);
    tcase_add_test(tc, test_strips_script_and_style);
    tcase_add_test(tc, test_decodes_entities);
    tcase_add_test(tc, test_literal_ampersand_preserved);
    tcase_add_test(tc, test_block_tags_become_newlines);
    tcase_add_test(tc, test_collapses_whitespace);
    tcase_add_test(tc, test_inline_tags_do_not_break_words);
    tcase_add_test(tc, test_title_is_prefixed);
    tcase_add_test(tc, test_citations_collected);
    tcase_add_test(tc, test_citation_deduplicated);
    tcase_add_test(tc, test_nav_class_blocks_pruned);
    tcase_add_test(tc, test_small_budget_truncates_with_marker);
    suite_add_tcase(s, tc);

    tc = tcase_create("Dispatch");
    tcase_add_test(tc, test_dispatch_html_by_type);
    tcase_add_test(tc, test_dispatch_sniffs_missing_type);
    tcase_add_test(tc, test_dispatch_plain_text_passthrough);
    tcase_add_test(tc, test_dispatch_json_truncated);
    tcase_add_test(tc, test_dispatch_binary_descriptor);
    suite_add_tcase(s, tc);

    tc = tcase_create("FaultInjection");
    tcase_add_test(tc, test_oom_allocation_failure);
    tcase_add_test(tc, test_oom_mid_parse_rolls_back);
    suite_add_tcase(s, tc);

    tc = tcase_create("Ellipsis");
    tcase_add_test(tc, test_ellipsis_short_passthrough);
    tcase_add_test(tc, test_ellipsis_truncates);
    tcase_add_test(tc, test_ellipsis_keeps_marker_when_room);
    suite_add_tcase(s, tc);

    tc = tcase_create("Encodings");
    tcase_add_test(tc, test_entities_decode_to_valid_utf8);
    tcase_add_test(tc, test_numeric_entities_keep_full_codepoint);
    tcase_add_test(tc, test_latin1_body_transcoded);
    tcase_add_test(tc, test_utf8_body_passthrough_unchanged);
    tcase_add_test(tc, test_title_entities_valid_utf8);
    tcase_add_test(tc, test_plain_text_latin1_sanitized);
    tcase_add_test(tc, test_truncation_does_not_split_utf8);
    tcase_add_test(tc, test_ellipsis_utf8_boundary);
    suite_add_tcase(s, tc);

    return s;
}
