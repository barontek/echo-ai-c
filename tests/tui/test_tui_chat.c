/*
 * test_tui_chat.c - scrollback model: block lifecycle, streaming commit
 * semantics, word wrapping, viewport clamping, and allocation-failure
 * injection at the block-append commit sites. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui_chat.h"

/* ---- fault-injection allocator shims (TUI_CHAT_TEST) ---- */
static int fail_at = -1;
static int call_count = 0;

void *tui_chat_test_calloc(size_t nmemb, size_t size)
{
    call_count++;
    if (call_count == fail_at) return NULL;
    return calloc(nmemb, size);
}

char *tui_chat_test_strdup(const char *s)
{
    call_count++;
    if (call_count == fail_at) return NULL;
    char *copy = malloc(strlen(s) + 1);
    if (!copy) return NULL;
    strcpy(copy, s);
    return copy;
}

static void set_fail_at(int n)
{
    fail_at = call_count + n;
}

/* ---- fixtures ---- */
static TuiChat *chat;

static void setup_chat(void)
{
    fail_at = -1;
    call_count = 0;
    chat = tui_chat_create();
    ck_assert_ptr_nonnull(chat);
}

static void teardown_chat(void)
{
    tui_chat_destroy(chat);
    chat = NULL;
}

/* ---- block lifecycle ---- */

START_TEST(test_user_assistant_blocks_append)
{
    ck_assert_int_eq(tui_chat_begin_user(chat, "hello there"), 0);
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_ASSISTANT), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "hi "), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "back"), 0);
    tui_chat_end_stream(chat);

    ck_assert_int_eq(tui_chat_block_count(chat), 2);
    ck_assert_int_eq(tui_chat_block_kind(chat, 0), TUI_BLOCK_USER);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "hello there");
    ck_assert_int_eq(tui_chat_block_kind(chat, 1), TUI_BLOCK_ASSISTANT);
    ck_assert_str_eq(tui_chat_block_text(chat, 1), "hi back");
}
END_TEST

START_TEST(test_begin_user_seals_open_stream)
{
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_THINK), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "thinking..."), 0);
    ck_assert_int_eq(tui_chat_begin_user(chat, "next"), 0);

    ck_assert_int_eq(tui_chat_block_count(chat), 2);
    ck_assert_int_eq(tui_chat_block_kind(chat, 0), TUI_BLOCK_THINK);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "thinking...");
    ck_assert_int_eq(tui_chat_block_kind(chat, 1), TUI_BLOCK_USER);
}
END_TEST

START_TEST(test_stream_append_noop_without_open_stream)
{
    ck_assert_int_eq(tui_chat_stream_append(chat, "orphan"), 0);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);
}
END_TEST

START_TEST(test_begin_stream_rejects_other_kinds)
{
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_TOOL), -1);
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_ERROR), -1);
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_USER), -1);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);
}
END_TEST

START_TEST(test_tool_and_error_blocks)
{
    ck_assert_int_eq(tui_chat_append_tool(chat, "bash", "done"), 0);
    ck_assert_int_eq(tui_chat_block_kind(chat, 0), TUI_BLOCK_TOOL);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "bash: done");
    ck_assert_int_eq(tui_chat_append_error(chat, "boom"), 0);
    ck_assert_int_eq(tui_chat_block_kind(chat, 1), TUI_BLOCK_ERROR);
    ck_assert_str_eq(tui_chat_block_text(chat, 1), "boom");
    /* NULL args tolerated */
    ck_assert_int_eq(tui_chat_append_tool(chat, NULL, NULL), 0);
    ck_assert_str_eq(tui_chat_block_text(chat, 2), "(tool): ");
}
END_TEST

START_TEST(test_out_of_range_accessors_are_safe)
{
    ck_assert_int_eq(tui_chat_block_count(chat), 0);
    ck_assert_int_eq(tui_chat_block_kind(chat, 0), TUI_BLOCK_ERROR);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "");
}
END_TEST

/* ---- wrapping ---- */

START_TEST(test_wrap_empty_text_is_one_line)
{
    size_t starts[4] = {999, 999, 999, 999};
    size_t n = tui_chat_wrap("", 10, starts, 4);
    ck_assert_int_eq(n, 1);
    ck_assert_int_eq(starts[0], 0);
}
END_TEST

START_TEST(test_wrap_short_text_single_line)
{
    size_t starts[4] = {999, 999, 999, 999};
    size_t n = tui_chat_wrap("hello world", 30, starts, 4);
    ck_assert_int_eq(n, 1);
    ck_assert_int_eq(starts[0], 0);
}
END_TEST

START_TEST(test_wrap_word_boundaries)
{
    size_t starts[8] = {0};
    /* width 5: "one two three" -> "one", "two", "three" */
    size_t n = tui_chat_wrap("one two three", 5, starts, 8);
    ck_assert_int_eq(n, 3);
    ck_assert_int_eq(starts[0], 0);
    ck_assert_int_eq(starts[1], 4);
    ck_assert_int_eq(starts[2], 8);
}
END_TEST

START_TEST(test_wrap_exact_fit_no_break)
{
    size_t starts[4] = {0};
    /* width 7: "abcdefg" fits exactly */
    size_t n = tui_chat_wrap("abcdefg", 7, starts, 4);
    ck_assert_int_eq(n, 1);
}
END_TEST

START_TEST(test_wrap_long_word_breaks_mid_word)
{
    size_t starts[8] = {0};
    /* width 3: "abcdefgh" -> "abc","def","gh" */
    size_t n = tui_chat_wrap("abcdefgh", 3, starts, 8);
    ck_assert_int_eq(n, 3);
    ck_assert_int_eq(starts[0], 0);
    ck_assert_int_eq(starts[1], 3);
    ck_assert_int_eq(starts[2], 6);
}
END_TEST

START_TEST(test_wrap_hard_breaks)
{
    size_t starts[8] = {0};
    /* "ab\ncdef" -> two lines regardless of width */
    size_t n = tui_chat_wrap("ab\ncdef", 100, starts, 8);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(starts[1], 3);
}
END_TEST

START_TEST(test_wrap_long_word_after_short_word)
{
    size_t starts[8] = {0};
    /* width 3: "ab cdefgh" -> "ab ", "cde", "fgh" */
    size_t n = tui_chat_wrap("ab cdefgh", 3, starts, 8);
    ck_assert_int_eq(n, 3);
    ck_assert_int_eq(starts[0], 0);
    ck_assert_int_eq(starts[1], 3);
    ck_assert_int_eq(starts[2], 6);
}
END_TEST

START_TEST(test_wrap_space_overflow_starts_new_line)
{
    size_t starts[8] = {0};
    /* width 3: "abc def" -> "abc", "def" (the space after abc wraps) */
    size_t n = tui_chat_wrap("abc def", 3, starts, 8);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(starts[1], 4);
}
END_TEST

START_TEST(test_wrap_cap_is_respected)
{
    size_t starts[2] = {999, 999};
    size_t n = tui_chat_wrap("one two three four", 5, starts, 2);
    ck_assert_int_eq(n, 4); /* true count reported */
    ck_assert_int_eq(starts[0], 0);
    ck_assert_int_eq(starts[1], 4); /* only 2 offsets written */
}
END_TEST

/* Locale-independent column widths: CJK chars are 2 columns, combining
 * marks 0 — regressions for the byte-counting wrapper that wrapped CJK
 * lines at a third of the width. */
START_TEST(test_wrap_counts_display_columns)
{
    size_t starts[8] = {0};
    /* width 4: two CJK chars (2 cols each) fit on one line, the third wraps */
    size_t n = tui_chat_wrap("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD", 4,
                             starts, 8);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(starts[0], 0);
    ck_assert_int_eq(starts[1], 6); /* third char starts a new line */
}
END_TEST

START_TEST(test_wrap_combining_marks_are_zero_width)
{
    /* "e" + U+0301 (combining acute) + "x": 2 columns total */
    size_t n = tui_chat_wrap("e\xCC\x81x", 2, NULL, 0);
    ck_assert_int_eq(n, 1);
    /* width 1 forces x to wrap after the e+mark cluster */
    size_t starts[4] = {0};
    n = tui_chat_wrap("e\xCC\x81x", 1, starts, 4);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(starts[1], 3);
}
END_TEST

START_TEST(test_wrap_lone_wide_codepoint_never_loops)
{
    /* A codepoint wider than the line is placed anyway (regression for
     * the infinite loop the byte-based wrapper could not hit) */
    size_t starts[8] = {0};
    size_t n = tui_chat_wrap("\xE4\xB8\xAD\xE4\xB8\xAD", 1, starts, 8);
    ck_assert_int_eq(n, 2); /* one char per line, no stall */
    ck_assert_int_eq(starts[0], 0);
    ck_assert_int_eq(starts[1], 3);
}
END_TEST

START_TEST(test_wrap_wide_word_moves_whole_word)
{
    /* width 6: "ab" (2) + space (1) leaves 3 cols; a 2-col CJK char does
     * not fit, so it moves to its own line */
    size_t starts[4] = {0};
    size_t n = tui_chat_wrap("ab \xE4\xB8\xAD\xE4\xB8\xAD", 6, starts, 4);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(starts[1], 3);
}
END_TEST

/* ---- wrap cache ---- */

START_TEST(test_line_starts_cache_reuses_and_invalidates)
{
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_ASSISTANT), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "one two three"), 0);
    tui_chat_end_stream(chat);

    size_t lines = 0;
    const size_t *s1 = tui_chat_block_line_starts(chat, 0, 5, &lines);
    ck_assert_ptr_nonnull(s1);
    ck_assert_int_eq(lines, 3);
    ck_assert_int_eq(s1[0], 0);
    ck_assert_int_eq(s1[1], 4);
    ck_assert_int_eq(s1[2], 8);
    ck_assert_int_eq(s1[3], 13); /* sentinel = text length */

    /* same width: cached array is reused (same pointer) */
    const size_t *s2 = tui_chat_block_line_starts(chat, 0, 5, &lines);
    ck_assert_ptr_eq(s1, s2);

    /* different width: recomputed */
    const size_t *s3 = tui_chat_block_line_starts(chat, 0, 2, &lines);
    ck_assert_ptr_nonnull(s3);
    ck_assert_int_gt(lines, 3);

    /* text mutation invalidates: begin a pending tool block, cache its
     * empty wrap, then fill the result and check the rewrap */
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), 0);
    (void)tui_chat_block_line_starts(chat, 1, 5, &lines);
    ck_assert_int_eq(lines, 1); /* empty text = one line */
    ck_assert_int_eq(tui_chat_tool_finish(chat, "bash", "one two three"), 0);
    const size_t *s4 = tui_chat_block_line_starts(chat, 1, 5, &lines);
    ck_assert_ptr_nonnull(s4);
    ck_assert_int_eq(lines, 3);
    ck_assert_int_eq(s4[1], 4);
    ck_assert_int_eq(s4[2], 8);
}
END_TEST

START_TEST(test_render_lines_uses_cached_wrap)
{
    /* render_lines must agree with the cache (marker math depends on it) */
    ck_assert_int_eq(tui_chat_begin_user(chat, "one two three four five"), 0);
    size_t lines = 0;
    (void)tui_chat_block_line_starts(chat, 0, 5, &lines);
    ck_assert_int_gt(lines, 1);
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, 5), 1 + lines);
}
END_TEST

/* ---- total lines + viewport ---- */

START_TEST(test_total_lines_with_separators)
{
    ck_assert_int_eq(tui_chat_begin_user(chat, "one two"), 0);        /* 2 lines at width 5 */
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_ASSISTANT), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "abc def ghi"), 0); /* 3 lines at width 5 */
    tui_chat_end_stream(chat);
    /* user: header+2, assistant: header+3, separators after each */
    ck_assert_int_eq(tui_chat_total_lines(chat, 5), 3 + 1 + 4 + 1);
}

START_TEST(test_total_lines_empty_is_one)
{
    ck_assert_int_eq(tui_chat_total_lines(chat, 10), 1);
}
END_TEST

START_TEST(test_view_clamp_bounds)
{
    /* 8 lines + header + trailing separator = 10 total at width 1 */
    ck_assert_int_eq(tui_chat_begin_user(chat, "a b c d e f g h"), 0);
    ck_assert_int_eq(tui_chat_view_clamp(chat, 1, 3, 100), 7); /* top = 10-3 */
    ck_assert_int_eq(tui_chat_view_clamp(chat, 1, 3, 2), 2);   /* in range */
    ck_assert_int_eq(tui_chat_view_clamp(chat, 1, 3, 0), 0);   /* bottom boundary */
    ck_assert_int_eq(tui_chat_view_clamp(chat, 1, 100, 3), 0); /* viewport larger than content */
}
END_TEST

/* ---- allocation-failure injection ---- */

START_TEST(test_append_fails_cleanly_on_array_growth)
{
    set_fail_at(1); /* first block: the blocks array realloc fails */
    ck_assert_int_eq(tui_chat_begin_user(chat, "text"), -1);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);
}
END_TEST

START_TEST(test_append_fails_cleanly_on_text_copy)
{
    set_fail_at(2); /* array ok, str_dup fails */
    ck_assert_int_eq(tui_chat_begin_user(chat, "text"), -1);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);
}
END_TEST

START_TEST(test_stream_failure_keeps_stream_open)
{
    /* Open a stream, then fail a begin_user that must seal it: the seal
     * must be rolled back so the streamed text survives. */
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_THINK), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "thoughts"), 0);
    set_fail_at(1);
    ck_assert_int_eq(tui_chat_begin_user(chat, "next"), -1);
    /* state unchanged: stream still open, text intact */
    ck_assert_int_eq(tui_chat_block_count(chat), 1);
    ck_assert_int_eq(tui_chat_block_kind(chat, 0), TUI_BLOCK_THINK);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "thoughts");
    /* and the stream still accepts appends */
    fail_at = -1;
    ck_assert_int_eq(tui_chat_stream_append(chat, " more"), 0);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "thoughts more");
}
END_TEST

START_TEST(test_append_failure_does_not_corrupt_prior_blocks)
{
    ck_assert_int_eq(tui_chat_begin_user(chat, "first"), 0);
    set_fail_at(1); /* next append's array growth fails */
    ck_assert_int_eq(tui_chat_begin_user(chat, "second"), -1);
    fail_at = -1;
    ck_assert_int_eq(tui_chat_block_count(chat), 1);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "first");
    ck_assert_int_eq(tui_chat_begin_user(chat, "third"), 0);
    ck_assert_int_eq(tui_chat_block_count(chat), 2);
    ck_assert_str_eq(tui_chat_block_text(chat, 1), "third");
}
END_TEST

START_TEST(test_stream_append_failure_preserves_text)
{
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_ASSISTANT), 0);
    ck_assert_int_eq(tui_chat_stream_append(chat, "abc"), 0);
    /* the realloc of the block text is one allocation after the two
     * appends above; force it to fail */
    set_fail_at(1);
    ck_assert_int_eq(tui_chat_stream_append(chat, "XYZ"), -1);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "abc");
}
END_TEST

/* ---- collapse/expand of long tool blocks ---- */

/* Build a tool result that wraps to more than the threshold lines. */
static char *make_long_tool_result(size_t width)
{
    char *text = malloc(4096);
    if (!text) return NULL;
    size_t o = 0;
    /* "aaaa bbbb cccc ..." repeated until it exceeds the threshold */
    for (int i = 0; o < sizeof("word ") * 60 - 1 && o < 4095; i++)
    {
        int n = snprintf(text + o, 4096 - o, "word%d ", i);
        if (n < 0) break;
        o += (size_t)n;
        if (tui_chat_wrap(text, width, NULL, 0) > TUI_CHAT_COLLAPSE_THRESHOLD)
            break;
    }
    return text;
}

START_TEST(test_long_tool_block_auto_collapses)
{
    size_t width = 20;
    char *text = make_long_tool_result(width);
    ck_assert_ptr_nonnull(text);
    ck_assert_int_gt(tui_chat_wrap(text, width, NULL, 0),
                     TUI_CHAT_COLLAPSE_THRESHOLD);

    ck_assert_int_eq(tui_chat_append_tool(chat, "bash", text), 0);
    ck_assert_int_eq(tui_chat_block_effective_collapsed(chat, 0, width), 1);
    /* header + content + marker row */
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, width),
                     1 + TUI_CHAT_COLLAPSE_THRESHOLD + 1);
    free(text);
}
END_TEST

START_TEST(test_toggle_expands_then_collapses)
{
    size_t width = 20;
    char *text = make_long_tool_result(width);
    ck_assert_ptr_nonnull(text);
    size_t full = tui_chat_wrap(text, width, NULL, 0);
    ck_assert_int_gt(full, TUI_CHAT_COLLAPSE_THRESHOLD);
    ck_assert_int_eq(tui_chat_append_tool(chat, "bash", text), 0);

    /* first toggle: AUTO(collapsed) -> expanded */
    ck_assert_int_eq(tui_chat_toggle_collapse(chat, 0, width), TUI_COLLAPSE_OFF);
    ck_assert_int_eq(tui_chat_block_effective_collapsed(chat, 0, width), 0);
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, width), 1 + full + 1);

    /* second toggle: expanded -> collapsed */
    ck_assert_int_eq(tui_chat_toggle_collapse(chat, 0, width), TUI_COLLAPSE_ON);
    ck_assert_int_eq(tui_chat_block_effective_collapsed(chat, 0, width), 1);
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, width),
                     1 + TUI_CHAT_COLLAPSE_THRESHOLD + 1);
    free(text);
}
END_TEST

START_TEST(test_short_tool_block_never_truncates)
{
    ck_assert_int_eq(tui_chat_append_tool(chat, "bash", "done"), 0);
    ck_assert_int_eq(tui_chat_block_effective_collapsed(chat, 0, 20), 0);
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, 20), 2); /* header+1 */
    /* toggle is a no-op for short blocks */
    ck_assert_int_eq(tui_chat_toggle_collapse(chat, 0, 20), TUI_COLLAPSE_AUTO);
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, 20), 2);
}
END_TEST

START_TEST(test_long_non_tool_block_never_truncates)
{
    /* a long assistant message stays fully visible */
    ck_assert_int_eq(tui_chat_begin_stream(chat, TUI_BLOCK_ASSISTANT), 0);
    for (int i = 0; i < 40; i++)
        ck_assert_int_eq(tui_chat_stream_append(chat, "word "), 0);
    tui_chat_end_stream(chat);
    size_t lines = tui_chat_block_render_lines(chat, 0, 10);
    ck_assert_int_gt(lines, TUI_CHAT_COLLAPSE_THRESHOLD);
    ck_assert_int_eq(tui_chat_block_effective_collapsed(chat, 0, 10), 0);
    ck_assert_int_eq(tui_chat_toggle_collapse(chat, 0, 10), TUI_COLLAPSE_AUTO);
    ck_assert_int_eq(tui_chat_block_render_lines(chat, 0, 10), lines);
}
END_TEST

START_TEST(test_total_lines_counts_marker_rows)
{
    size_t width = 20;
    char *text = make_long_tool_result(width);
    ck_assert_ptr_nonnull(text);
    size_t full = tui_chat_wrap(text, width, NULL, 0);
    ck_assert_int_gt(full, TUI_CHAT_COLLAPSE_THRESHOLD);

    ck_assert_int_eq(tui_chat_append_tool(chat, "bash", text), 0);
    /* collapsed: header+content+marker + trailing separator */
    ck_assert_int_eq(tui_chat_total_lines(chat, width),
                     1 + TUI_CHAT_COLLAPSE_THRESHOLD + 1 + 1);
    /* expanded: header+full+marker + trailing separator */
    (void)tui_chat_toggle_collapse(chat, 0, width);
    ck_assert_int_eq(tui_chat_total_lines(chat, width), 1 + full + 1 + 1);
    free(text);
}
END_TEST

/* ---- pending tool blocks (begin/finish) ---- */

START_TEST(test_begin_tool_creates_pending_block)
{
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), 0);
    ck_assert_int_eq(tui_chat_block_count(chat), 1);
    ck_assert_int_eq(tui_chat_block_kind(chat, 0), TUI_BLOCK_TOOL);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "");
    ck_assert_str_eq(tui_chat_block_title(chat, 0), "bash");
}
END_TEST

START_TEST(test_tool_finish_fills_pending_block)
{
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), 0);
    ck_assert_int_eq(tui_chat_tool_finish(chat, "bash", "Exit code: 0\n/home/x"), 0);
    /* one block, now with the result — the tool never appears twice */
    ck_assert_int_eq(tui_chat_block_count(chat), 1);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "Exit code: 0\n/home/x");
    ck_assert_str_eq(tui_chat_block_title(chat, 0), "bash");
}
END_TEST

START_TEST(test_tool_finish_without_pending_appends)
{
    /* a missed start event falls back to the classic "<name>: <result>" */
    ck_assert_int_eq(tui_chat_tool_finish(chat, "bash", "out"), 0);
    ck_assert_int_eq(tui_chat_block_count(chat), 1);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "bash: out");
    ck_assert_ptr_null(tui_chat_block_title(chat, 0));
}
END_TEST

START_TEST(test_begin_tool_fault_injection)
{
    set_fail_at(1); /* blocks array growth (cap==0 on a fresh chat) */
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), -1);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);

    set_fail_at(2); /* array ok, text copy fails */
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), -1);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);

    /* the array now exists, so a call allocates text then title: the
     * second allocation is the title copy, which must roll back text */
    set_fail_at(2);
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), -1);
    ck_assert_int_eq(tui_chat_block_count(chat), 0);

    /* normal operation afterwards */
    fail_at = -1;
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), 0);
    ck_assert_int_eq(tui_chat_block_count(chat), 1);
}
END_TEST

START_TEST(test_tool_finish_failure_keeps_pending)
{
    ck_assert_int_eq(tui_chat_begin_tool(chat, "bash"), 0);
    set_fail_at(1); /* the result copy fails: block stays pending */
    ck_assert_int_eq(tui_chat_tool_finish(chat, "bash", "result"), -1);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "");
    fail_at = -1;
    ck_assert_int_eq(tui_chat_tool_finish(chat, "bash", "result"), 0);
    ck_assert_str_eq(tui_chat_block_text(chat, 0), "result");
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tui_chat");
    TCase *tc_life = tcase_create("lifecycle");
    tcase_add_checked_fixture(tc_life, setup_chat, teardown_chat);
    tcase_add_test(tc_life, test_user_assistant_blocks_append);
    tcase_add_test(tc_life, test_begin_user_seals_open_stream);
    tcase_add_test(tc_life, test_stream_append_noop_without_open_stream);
    tcase_add_test(tc_life, test_begin_stream_rejects_other_kinds);
    tcase_add_test(tc_life, test_tool_and_error_blocks);
    tcase_add_test(tc_life, test_out_of_range_accessors_are_safe);
    suite_add_tcase(s, tc_life);

    TCase *tc_wrap = tcase_create("wrap");
    tcase_add_checked_fixture(tc_wrap, setup_chat, teardown_chat);
    tcase_add_test(tc_wrap, test_wrap_empty_text_is_one_line);
    tcase_add_test(tc_wrap, test_wrap_short_text_single_line);
    tcase_add_test(tc_wrap, test_wrap_word_boundaries);
    tcase_add_test(tc_wrap, test_wrap_exact_fit_no_break);
    tcase_add_test(tc_wrap, test_wrap_long_word_breaks_mid_word);
    tcase_add_test(tc_wrap, test_wrap_hard_breaks);
    tcase_add_test(tc_wrap, test_wrap_long_word_after_short_word);
    tcase_add_test(tc_wrap, test_wrap_space_overflow_starts_new_line);
    tcase_add_test(tc_wrap, test_wrap_cap_is_respected);
    tcase_add_test(tc_wrap, test_wrap_counts_display_columns);
    tcase_add_test(tc_wrap, test_wrap_combining_marks_are_zero_width);
    tcase_add_test(tc_wrap, test_wrap_lone_wide_codepoint_never_loops);
    tcase_add_test(tc_wrap, test_wrap_wide_word_moves_whole_word);
    tcase_add_test(tc_wrap, test_total_lines_with_separators);
    tcase_add_test(tc_wrap, test_total_lines_empty_is_one);
    tcase_add_test(tc_wrap, test_view_clamp_bounds);
    tcase_add_test(tc_wrap, test_line_starts_cache_reuses_and_invalidates);
    tcase_add_test(tc_wrap, test_render_lines_uses_cached_wrap);
    suite_add_tcase(s, tc_wrap);

    TCase *tc_fault = tcase_create("fault_injection");
    tcase_add_checked_fixture(tc_fault, setup_chat, teardown_chat);
    tcase_add_test(tc_fault, test_append_fails_cleanly_on_array_growth);
    tcase_add_test(tc_fault, test_append_fails_cleanly_on_text_copy);
    tcase_add_test(tc_fault, test_stream_failure_keeps_stream_open);
    tcase_add_test(tc_fault, test_append_failure_does_not_corrupt_prior_blocks);
    tcase_add_test(tc_fault, test_stream_append_failure_preserves_text);
    suite_add_tcase(s, tc_fault);

    TCase *tc_collapse = tcase_create("collapse");
    tcase_add_checked_fixture(tc_collapse, setup_chat, teardown_chat);
    tcase_add_test(tc_collapse, test_long_tool_block_auto_collapses);
    tcase_add_test(tc_collapse, test_toggle_expands_then_collapses);
    tcase_add_test(tc_collapse, test_short_tool_block_never_truncates);
    tcase_add_test(tc_collapse, test_long_non_tool_block_never_truncates);
    tcase_add_test(tc_collapse, test_total_lines_counts_marker_rows);
    suite_add_tcase(s, tc_collapse);

    TCase *tc_tool = tcase_create("pending_tool");
    tcase_add_checked_fixture(tc_tool, setup_chat, teardown_chat);
    tcase_add_test(tc_tool, test_begin_tool_creates_pending_block);
    tcase_add_test(tc_tool, test_tool_finish_fills_pending_block);
    tcase_add_test(tc_tool, test_tool_finish_without_pending_appends);
    tcase_add_test(tc_tool, test_begin_tool_fault_injection);
    tcase_add_test(tc_tool, test_tool_finish_failure_keeps_pending);
    suite_add_tcase(s, tc_tool);

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
