/*
 * test_markdown.c - markdown classification for the chat pane: inline
 * run tokenization (bold/italic/code/strike/links/escapes), line kind
 * detection, table geometry + row rendering, and the shared display
 * width helper. Pure logic — no terminal involved. Depends on: check.
 */

#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "tui/markdown.h"
#include "tui/tui_chat.h"

#define MAX_RUNS 64

/* Parse a NUL-terminated line into runs. */
static size_t parse(const char *line, MdRun *runs)
{
    return md_parse_inline(line, strlen(line), runs, MAX_RUNS);
}

/* Assert one run's position, style and (optionally) text. */
static void assert_run(const char *line, const MdRun *r,
                       size_t start, size_t len, unsigned style,
                       const char *text)
{
    ck_assert_uint_eq(r->start, start);
    ck_assert_uint_eq(r->len, len);
    ck_assert_uint_eq(r->style, style);
    if (text)
    {
        ck_assert_uint_le(start + len, strlen(line));
        ck_assert_mem_eq(line + start, text, len);
    }
}

START_TEST(test_inline_plain_single_run)
{
    MdRun runs[MAX_RUNS];
    const char *line = "hello world";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 0, 11, 0, "hello world");
}
END_TEST

START_TEST(test_inline_bold)
{
    MdRun runs[MAX_RUNS];
    const char *line = "**bold**";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 2, 4, MD_STYLE_BOLD, "bold");
}
END_TEST

START_TEST(test_inline_bold_underscore)
{
    MdRun runs[MAX_RUNS];
    const char *line = "a __bold__ c";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 3);
    assert_run(line, &runs[0], 0, 2, 0, "a ");
    assert_run(line, &runs[1], 4, 4, MD_STYLE_BOLD, "bold");
    assert_run(line, &runs[2], 10, 2, 0, " c");
}
END_TEST

START_TEST(test_inline_italic)
{
    MdRun runs[MAX_RUNS];
    const char *line = "*it*";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 1, 2, MD_STYLE_ITALIC, "it");
}
END_TEST

START_TEST(test_inline_bold_italic)
{
    MdRun runs[MAX_RUNS];
    const char *line = "***both***";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    ck_assert_uint_eq(runs[0].style,
                      MD_STYLE_BOLD | MD_STYLE_ITALIC);
    assert_run(line, &runs[0], 3, 4, MD_STYLE_BOLD | MD_STYLE_ITALIC, "both");
}
END_TEST

START_TEST(test_inline_mixed_styles_in_one_line)
{
    MdRun runs[MAX_RUNS];
    const char *line = "a **b** c";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 3);
    assert_run(line, &runs[0], 0, 2, 0, "a ");
    assert_run(line, &runs[1], 4, 1, MD_STYLE_BOLD, "b");
    assert_run(line, &runs[2], 7, 2, 0, " c");
}
END_TEST

START_TEST(test_inline_nested_bold_italic)
{
    MdRun runs[MAX_RUNS];
    const char *line = "**a *b* c**";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 3);
    assert_run(line, &runs[0], 2, 2, MD_STYLE_BOLD, "a ");
    assert_run(line, &runs[1], 5, 1, MD_STYLE_BOLD | MD_STYLE_ITALIC, "b");
    assert_run(line, &runs[2], 7, 2, MD_STYLE_BOLD, " c");
}
END_TEST

START_TEST(test_inline_snake_case_underscores_survive)
{
    MdRun runs[MAX_RUNS];
    const char *line = "foo_bar_baz";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 0, 11, 0, NULL);
}
END_TEST

START_TEST(test_inline_asterisks_around_numbers_literal)
{
    MdRun runs[MAX_RUNS];
    const char *line = "2 * 3 * 4";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 0, 9, 0, NULL);
}
END_TEST

START_TEST(test_inline_unclosed_delimiter_is_literal)
{
    MdRun runs[MAX_RUNS];
    const char *line = "**bold";
    size_t n = parse(line, runs);
    /* no closer on the line: the asterisks must not be consumed, so a
     * half-streamed response renders as text, not mystery styling */
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 0, 6, 0, NULL);
}
END_TEST

START_TEST(test_inline_underscore_italic_word_boundaries)
{
    MdRun runs[MAX_RUNS];
    const char *line = "_word_";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 1, 4, MD_STYLE_ITALIC, "word");
}
END_TEST

START_TEST(test_inline_code_span_is_literal)
{
    MdRun runs[MAX_RUNS];
    const char *line = "`a *b* c`";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 1, 7, MD_STYLE_CODE, "a *b* c");
}
END_TEST

START_TEST(test_inline_code_span_backtick_mismatch_stays_literal)
{
    MdRun runs[MAX_RUNS];
    const char *line = "`a``b`";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 1, 4, MD_STYLE_CODE, "a``b");
}
END_TEST

START_TEST(test_inline_code_inside_bold)
{
    MdRun runs[MAX_RUNS];
    const char *line = "**a `b` c**";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 3);
    assert_run(line, &runs[0], 2, 2, MD_STYLE_BOLD, "a ");
    assert_run(line, &runs[1], 5, 1, MD_STYLE_BOLD | MD_STYLE_CODE, "b");
    assert_run(line, &runs[2], 7, 2, MD_STYLE_BOLD, " c");
}
END_TEST

START_TEST(test_inline_strike)
{
    MdRun runs[MAX_RUNS];
    const char *line = "~~gone~~";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 2, 4, MD_STYLE_STRIKE, "gone");
}
END_TEST

START_TEST(test_inline_link_renders_text_only)
{
    MdRun runs[MAX_RUNS];
    const char *line = "see [docs](https://example.com) now";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 3);
    assert_run(line, &runs[0], 0, 4, 0, "see ");
    assert_run(line, &runs[1], 5, 4, MD_STYLE_LINK, "docs");
    assert_run(line, &runs[2], 31, 4, 0, " now");
}
END_TEST

START_TEST(test_inline_malformed_link_is_literal)
{
    MdRun runs[MAX_RUNS];
    const char *line = "a [broken]";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 1);
    assert_run(line, &runs[0], 0, 10, 0, NULL);
}
END_TEST

START_TEST(test_inline_backslash_escape)
{
    MdRun runs[MAX_RUNS];
    const char *line = "\\*not\\*";
    size_t n = parse(line, runs);
    ck_assert_uint_eq(n, 3);
    assert_run(line, &runs[0], 1, 1, 0, "*");
    assert_run(line, &runs[1], 2, 3, 0, "not");
    assert_run(line, &runs[2], 6, 1, 0, "*");
}
END_TEST

START_TEST(test_line_kind_heading)
{
    ck_assert_int_eq(md_line_kind("## heading", 10), MD_LINE_HEADING);
    ck_assert_int_eq(md_line_kind("#", 1), MD_LINE_HEADING);
    ck_assert_int_eq(md_line_kind("  ### h", 8), MD_LINE_HEADING);
    ck_assert_int_eq(md_line_kind("###x", 4), MD_LINE_PLAIN);
    ck_assert_int_eq(md_line_kind("####### x", 9), MD_LINE_PLAIN);
    ck_assert_int_eq(md_line_kind("    ## h", 8), MD_LINE_PLAIN);
}
END_TEST

START_TEST(test_line_kind_quote)
{
    ck_assert_int_eq(md_line_kind("> quoted", 8), MD_LINE_QUOTE);
    ck_assert_int_eq(md_line_kind(">", 1), MD_LINE_QUOTE);
}
END_TEST

START_TEST(test_line_kind_list)
{
    ck_assert_int_eq(md_line_kind("- item", 6), MD_LINE_LIST);
    ck_assert_int_eq(md_line_kind("* item", 6), MD_LINE_LIST);
    ck_assert_int_eq(md_line_kind("+ item", 6), MD_LINE_LIST);
    ck_assert_int_eq(md_line_kind("1. item", 8), MD_LINE_LIST);
    ck_assert_int_eq(md_line_kind("10) item", 9), MD_LINE_LIST);
    ck_assert_int_eq(md_line_kind("1.2", 3), MD_LINE_PLAIN);
    ck_assert_int_eq(md_line_kind("-", 1), MD_LINE_PLAIN);
}
END_TEST

START_TEST(test_line_kind_hr)
{
    ck_assert_int_eq(md_line_kind("---", 3), MD_LINE_HR);
    ck_assert_int_eq(md_line_kind("***", 3), MD_LINE_HR);
    ck_assert_int_eq(md_line_kind("___", 3), MD_LINE_HR);
    ck_assert_int_eq(md_line_kind("---x", 4), MD_LINE_PLAIN);
    ck_assert_int_eq(md_line_kind("--", 2), MD_LINE_PLAIN);
}
END_TEST

START_TEST(test_line_kind_fence)
{
    ck_assert_int_eq(md_line_kind("```python", 9), MD_LINE_CODE_FENCE);
    ck_assert_int_eq(md_line_kind("~~~", 3), MD_LINE_CODE_FENCE);
    ck_assert_int_eq(md_line_kind("``", 2), MD_LINE_PLAIN);
}
END_TEST

START_TEST(test_line_kind_table)
{
    ck_assert_int_eq(md_line_kind("| a | b |", 9), MD_LINE_TABLE_ROW);
    ck_assert_int_eq(md_line_kind("| --- | :--: |", 14), MD_LINE_TABLE_SEP);
    ck_assert_int_eq(md_line_kind("|", 1), MD_LINE_PLAIN);
    ck_assert_int_eq(md_line_kind("a | b", 5), MD_LINE_PLAIN);
}
END_TEST

START_TEST(test_line_kind_blank_and_plain)
{
    ck_assert_int_eq(md_line_kind("", 0), MD_LINE_BLANK);
    ck_assert_int_eq(md_line_kind("   ", 3), MD_LINE_BLANK);
    ck_assert_int_eq(md_line_kind("hello", 5), MD_LINE_PLAIN);
}
END_TEST

/* ---- table geometry ---- */

/* Fill line-start offsets by scanning the text; the sentinel is the
 * total length. Avoids hand-counted byte offsets in the tests. */
static void line_starts(const char *text, size_t *starts, size_t nlines)
{
    size_t pos = 0;
    for (size_t i = 0; i < nlines; i++)
    {
        starts[i] = pos;
        while (text[pos] && text[pos] != '\n') pos++;
        if (text[pos]) pos++; /* skip the newline */
    }
    starts[nlines] = pos;
}

START_TEST(test_table_scan_basic_geometry)
{
    const char *text = "| a | bb |\n|:--|--:|\n| 1 | 2  |\n| 3 | 4 |\n";
    size_t starts[5];
    line_starts(text, starts, 4);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 4, 0, &rows);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(t->ncols, 2);
    ck_assert_uint_eq(t->nrows, 3);
    ck_assert_uint_eq(rows, 3);
    ck_assert_uint_eq(t->widths[0], 1); /* a / 1 */
    ck_assert_uint_eq(t->widths[1], 2); /* bb / 2 */
    ck_assert_uint_eq(t->align[0], 0);  /* :--  = left */
    ck_assert_uint_eq(t->align[1], 2);  /* --:  = right */
    md_table_free(t);
}
END_TEST

START_TEST(test_table_scan_center_alignment)
{
    const char *text = "| h |\n|:-:|\n| x |\n";
    size_t starts[4];
    line_starts(text, starts, 3);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 3, 0, &rows);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(t->ncols, 1);
    ck_assert_uint_eq(t->align[0], 1);
    md_table_free(t);
}
END_TEST

START_TEST(test_table_scan_ragged_rows)
{
    /* second row has fewer cells: treated as empty trailing cells */
    const char *text = "| a | b |\n|---|---|\n| c |\n";
    size_t starts[4];
    line_starts(text, starts, 3);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 3, 0, &rows);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(t->ncols, 2);
    ck_assert_uint_eq(t->widths[1], 1);
    md_table_free(t);
}
END_TEST

START_TEST(test_table_scan_not_at_first_line)
{
    /* the separator sits between the first row and the rest, so row
     * indexing is non-contiguous — a table starting after line 0
     * exercises that path */
    const char *text = "intro\n| a | b |\n|:--|--:|\n| x | y |\n";
    size_t starts[5];
    line_starts(text, starts, 4);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 4, 1, &rows);
    ck_assert_ptr_nonnull(t);
    ck_assert_uint_eq(t->nrows, 2);
    ck_assert_uint_eq(rows, 2);
    ck_assert_uint_eq(t->widths[0], 1);
    ck_assert_uint_eq(t->widths[1], 1);
    md_table_free(t);
}
END_TEST

START_TEST(test_table_scan_rejects_missing_separator)
{
    const char *text = "| a | b |\nplain text\n";
    size_t starts[3];
    line_starts(text, starts, 2);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 2, 0, &rows);
    ck_assert_ptr_null(t); /* no separator: falls back to plain text */
}
END_TEST

START_TEST(test_table_scan_rejects_non_row_start)
{
    const char *text = "hello\n| --- |\n";
    size_t starts[3];
    line_starts(text, starts, 2);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 2, 0, &rows);
    ck_assert_ptr_null(t);
}
END_TEST

START_TEST(test_table_render_row_pads_and_aligns)
{
    const char *text = "| a | bb |\n|:--|--:|\n| 3 | 4 |\n";
    size_t starts[4];
    line_starts(text, starts, 3);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 3, 0, &rows);
    ck_assert_ptr_nonnull(t);

    char out[64];
    size_t n = md_table_render_row(out, sizeof(out), "| 3 | 4 |", 8, t);
    ck_assert_str_eq(out, "\xE2\x94\x82 3 \xE2\x94\x82  4 \xE2\x94\x82");
    ck_assert_uint_eq(n, strlen(out));

    n = md_table_render_row(out, sizeof(out), "| a | bb |", 9, t);
    ck_assert_str_eq(out, "\xE2\x94\x82 a \xE2\x94\x82 bb \xE2\x94\x82");
    ck_assert_uint_eq(n, strlen(out));
    md_table_free(t);
}
END_TEST

START_TEST(test_table_render_escaped_pipe)
{
    const char *text = "| a\\|b | c |\n|---|---|\n| 1 | 2 |\n";
    size_t starts[4];
    line_starts(text, starts, 3);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 3, 0, &rows);
    ck_assert_ptr_nonnull(t);
    char out[64];
    size_t n = md_table_render_row(out, sizeof(out), "| a\\|b | c |", 12, t);
    ck_assert_str_eq(out, "\xE2\x94\x82 a|b \xE2\x94\x82 c \xE2\x94\x82");
    ck_assert_uint_eq(n, strlen(out));
    md_table_free(t);
}
END_TEST

START_TEST(test_table_render_truncates_at_cap)
{
    const char *text = "| a | bb |\n|---|---|\n";
    size_t starts[3];
    line_starts(text, starts, 2);
    size_t rows = 0;
    MdTable *t = md_table_scan(text, starts, 2, 0, &rows);
    ck_assert_ptr_nonnull(t);
    char out[8];
    size_t n = md_table_render_row(out, sizeof(out), "| a | bb |", 9, t);
    ck_assert_uint_lt(n, sizeof(out));
    out[sizeof(out) - 1] = '\0';
    ck_assert_uint_eq(strlen(out), sizeof(out) - 1);
    md_table_free(t);
}
END_TEST

/* ---- display width (shared with the chat wrapper) ---- */

START_TEST(test_display_width_ascii)
{
    ck_assert_uint_eq(tui_chat_display_width("hello", 5), 5);
}
END_TEST

START_TEST(test_display_width_wide_and_combining)
{
    /* CJK is 2 columns, combining marks 0: "hé" + combining acute */
    ck_assert_uint_eq(tui_chat_display_width("\xE4\xBD\xA0\xE5\xA5\xBD", 6), 4);
    ck_assert_uint_eq(tui_chat_display_width("e\xCC\x81", 3), 1);
    ck_assert_uint_eq(tui_chat_display_width("\xF0\x9F\x98\x80", 4), 2); /* emoji */
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("markdown");
    TCase *tc = tcase_create("inline");
    tcase_add_test(tc, test_inline_plain_single_run);
    tcase_add_test(tc, test_inline_bold);
    tcase_add_test(tc, test_inline_bold_underscore);
    tcase_add_test(tc, test_inline_italic);
    tcase_add_test(tc, test_inline_bold_italic);
    tcase_add_test(tc, test_inline_mixed_styles_in_one_line);
    tcase_add_test(tc, test_inline_nested_bold_italic);
    tcase_add_test(tc, test_inline_snake_case_underscores_survive);
    tcase_add_test(tc, test_inline_asterisks_around_numbers_literal);
    tcase_add_test(tc, test_inline_unclosed_delimiter_is_literal);
    tcase_add_test(tc, test_inline_underscore_italic_word_boundaries);
    tcase_add_test(tc, test_inline_code_span_is_literal);
    tcase_add_test(tc, test_inline_code_span_backtick_mismatch_stays_literal);
    tcase_add_test(tc, test_inline_code_inside_bold);
    tcase_add_test(tc, test_inline_strike);
    tcase_add_test(tc, test_inline_link_renders_text_only);
    tcase_add_test(tc, test_inline_malformed_link_is_literal);
    tcase_add_test(tc, test_inline_backslash_escape);
    suite_add_tcase(s, tc);

    TCase *tc_line = tcase_create("lines");
    tcase_add_test(tc_line, test_line_kind_heading);
    tcase_add_test(tc_line, test_line_kind_quote);
    tcase_add_test(tc_line, test_line_kind_list);
    tcase_add_test(tc_line, test_line_kind_hr);
    tcase_add_test(tc_line, test_line_kind_fence);
    tcase_add_test(tc_line, test_line_kind_table);
    tcase_add_test(tc_line, test_line_kind_blank_and_plain);
    suite_add_tcase(s, tc_line);

    TCase *tc_table = tcase_create("tables");
    tcase_add_test(tc_table, test_table_scan_basic_geometry);
    tcase_add_test(tc_table, test_table_scan_center_alignment);
    tcase_add_test(tc_table, test_table_scan_ragged_rows);
    tcase_add_test(tc_table, test_table_scan_not_at_first_line);
    tcase_add_test(tc_table, test_table_scan_rejects_missing_separator);
    tcase_add_test(tc_table, test_table_scan_rejects_non_row_start);
    tcase_add_test(tc_table, test_table_render_row_pads_and_aligns);
    tcase_add_test(tc_table, test_table_render_escaped_pipe);
    tcase_add_test(tc_table, test_table_render_truncates_at_cap);
    suite_add_tcase(s, tc_table);

    TCase *tc_width = tcase_create("width");
    tcase_add_test(tc_width, test_display_width_ascii);
    tcase_add_test(tc_width, test_display_width_wide_and_combining);
    suite_add_tcase(s, tc_width);
    return s;
}

int main(void)
{
    Suite *s = suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
