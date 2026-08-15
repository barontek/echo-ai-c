/*
 * test_edit.c - unit tests for the edit tool: unique-match replacement,
 * multi-edit batching, uniqueness enforcement, overlap/no-match/no-op
 * errors, line-ending/BOM preservation, and fault injection on the
 * multi-edit commit path. Depends on: check, tool.h, safety, config.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "utils/string_utils.h"
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_edit_create(SafetyConfig *safety);
void edit_test_set_fwrite_fail(int fail);
void edit_test_set_strdup_fail(int call);

static SafetyConfig *make_safety(const char *workspace)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(workspace);
    safety->max_file_size = 4096;
    return safety;
}

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    ck_assert_ptr_nonnull(buf);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    ck_assert_ptr_nonnull(f);
    fputs(content, f);
    fclose(f);
}

START_TEST(test_edit_replaces_unique_occurrence)
{
    char ws[] = "/tmp/echo_rif_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "foo bar foo");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    /* T0: the tool registers under the pi-compatible contract name. */
    ck_assert_str_eq(tool->name, "edit");
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"bar\",\"new_string\":\"X\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content,
        "Replaced 1 occurrence (9 bytes written).\n\n"
        "-1 foo bar foo\n"
        "+1 foo X foo");
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "foo X foo");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_no_match_is_validation_error)
{
    /* T3: a no-match is an error naming the file (pi semantics), not a
     * success notice — the model must learn the old_string was wrong. */
    char ws[] = "/tmp/echo_rif_nomatch_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "hello");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"zzz\",\"new_string\":\"X\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    ck_assert_ptr_nonnull(strstr(r->error, "Could not find the exact text in f.txt"));
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "hello");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_duplicate_old_string_reports_count)
{
    /* T3 regression: "foo" appears twice — the tool must refuse with
     * the occurrence count instead of silently replacing the first
     * occurrence (the pre-T3 behavior). */
    char ws[] = "/tmp/echo_rif_dup_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "foo bar foo");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"foo\",\"new_string\":\"X\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    ck_assert_ptr_nonnull(strstr(r->error, "Found 2 occurrences"));
    ck_assert_ptr_nonnull(strstr(r->error, "make it unique"));
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "foo bar foo");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_multiple_disjoint_edits)
{
    /* T3: three disjoint edits in one call; each is matched against the
     * original content and applied in reverse. */
    char ws[] = "/tmp/echo_rif_multi_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "aaa BBB ccc ddd");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"edits\":["
        "{\"old_string\":\"aaa\",\"new_string\":\"1\"},"
        "{\"old_string\":\"BBB\",\"new_string\":\"2\"},"
        "{\"old_string\":\"ddd\",\"new_string\":\"3\"}]}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content,
        "Replaced 3 occurrences (9 bytes written).\n\n"
        "-1 aaa BBB ccc ddd\n"
        "+1 1 2 ccc 3");
    tool_result_free(r);
    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "1 2 ccc 3");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_diff_shows_context_and_line_numbers)
{
    /* opencode-style result: the diff shows the change with context
     * lines, '-'/'+' markers, line numbers, and " ..." for skipped
     * runs — editing line 4 of a 7-line file must show lines 1-3 as
     * context, -4/+4 as the change, and lines 5-7 as trailing context
     * with the final empty line skipped. */
    char ws[] = "/tmp/echo_rif_diff_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "l1\nl2\nl3\nl4\nl5\nl6\nl7\n");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"l4\",\"new_string\":\"L4\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content, "Replaced 1 occurrence"));
    ck_assert_ptr_nonnull(strstr(r->content,
        "\n\n 1 l1\n"
        " 2 l2\n"
        " 3 l3\n"
        "-4 l4\n"
        "+4 L4\n"
        " 5 l5\n"
        " 6 l6\n"
        " 7 l7"));
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_diff_context_capped_above_and_below)
{
    /* a change deep in a long file must show only 3 context lines
     * immediately above it and 3 below, with no " ..." markers — the
     * pre-fix output showed 6 above (3 + " ..." + 3) and a trailing
     * " ...". */
    char ws[] = "/tmp/echo_rif_ctx_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    for (int i = 1; i <= 20; i++) fprintf(f, "l%d\n", i);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"l12\",\"new_string\":\"L12\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content,
        "\n\n"
        " 9 l9\n"
        " 10 l10\n"
        " 11 l11\n"
        "-12 l12\n"
        "+12 L12\n"
        " 13 l13\n"
        " 14 l14\n"
        " 15 l15"));
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_multiple_hunks_each_with_context)
{
    /* multiple edits in one call render as separate hunks, each with
     * its own 3 context lines above and below — the pre-fix output
     * swallowed the context below every interior change into the
     * " ..." marker */
    char ws[] = "/tmp/echo_rif_hunks_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    for (int i = 1; i <= 60; i++) fprintf(f, "alpha%d\n", i);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"edits\":["
        "{\"old_string\":\"alpha5\\n\",\"new_string\":\"ALPHA FIVE\\n\"},"
        "{\"old_string\":\"alpha42\",\"new_string\":\"ALPHA 42\"},"
        "{\"old_string\":\"alpha58\",\"new_string\":\"ALPHA 58\"}]}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content,
        "Replaced 3 occurrences (477 bytes written).\n\n"
        " 1 alpha1\n"
        " 2 alpha2\n"
        " 3 alpha3\n"
        " 4 alpha4\n"
        "-5 alpha5\n"
        "+5 ALPHA FIVE\n"
        " 6 alpha6\n"
        " 7 alpha7\n"
        " 8 alpha8\n"
        " 39 alpha39\n"
        " 40 alpha40\n"
        " 41 alpha41\n"
        "-42 alpha42\n"
        "+42 ALPHA 42\n"
        " 43 alpha43\n"
        " 44 alpha44\n"
        " 45 alpha45\n"
        " 55 alpha55\n"
        " 56 alpha56\n"
        " 57 alpha57\n"
        "-58 alpha58\n"
        "+58 ALPHA 58\n"
        " 59 alpha59\n"
        " 60 alpha60\n"
        " 61 ");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_multiline_removal_shows_only_changed_lines)
{
    /* regression: a multi-line removal shifts the line alignment, and
     * the old greedy run collector swallowed the rest of the file —
     * the diff must show exactly the removed and added lines */
    char ws[] = "/tmp/echo_rif_ml_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path,
        "[tools]\n"
        "# comment one\n"
        "# comment two\n"
        "enabled = bash, read_file, write_file, edit, web_search, memory\n"
        "\n"
        "[search]\n"
        "provider = brave\n");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"# comment one\\n"
        "# comment two\\n"
        "enabled = bash, read_file, write_file, edit, web_search, memory\","
        "\"new_string\":\"enabled = bash, read_file, write_file, edit, web_search\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content,
        "Replaced 1 occurrence (91 bytes written).\n\n"
        " 1 [tools]\n"
        "-2 # comment one\n"
        "-3 # comment two\n"
        "-4 enabled = bash, read_file, write_file, edit, web_search, memory\n"
        "+2 enabled = bash, read_file, write_file, edit, web_search\n"
        " 5 \n"
        " 6 [search]\n"
        " 7 provider = brave");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_overlapping_edits_error)
{
    /* T3: overlapping edits are refused; the message names both
     * edit indices. */
    char ws[] = "/tmp/echo_rif_overlap_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "abcdef");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"edits\":["
        "{\"old_string\":\"abc\",\"new_string\":\"X\"},"
        "{\"old_string\":\"bcd\",\"new_string\":\"Y\"}]}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    ck_assert_ptr_nonnull(strstr(r->error, "overlap"));
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "abcdef");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_empty_old_string_error)
{
    char ws[] = "/tmp/echo_rif_empty_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "hello");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"\",\"new_string\":\"X\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_noop_reports_error)
{
    /* T3: replacing text with identical text is refused — the model
     * must learn the edit had no effect. */
    char ws[] = "/tmp/echo_rif_noop_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "hello world");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"world\",\"new_string\":\"world\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    ck_assert_ptr_nonnull(strstr(r->error, "identical content"));
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_edit_index_in_error_message)
{
    /* T3: with multiple edits, a failure names the offending index so
     * the model can fix exactly that edit. */
    char ws[] = "/tmp/echo_rif_idx_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "aaa bbb");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"edits\":["
        "{\"old_string\":\"aaa\",\"new_string\":\"1\"},"
        "{\"old_string\":\"zzz\",\"new_string\":\"2\"}]}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_ptr_nonnull(strstr(r->error, "edits[1]"));
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_crlf_preserved)
{
    /* T3a: editing a CRLF file keeps CRLF endings in the result. */
    char ws[] = "/tmp/echo_rif_crlf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "wb");
    ck_assert_ptr_nonnull(f);
    fputs("one\r\ntwo\r\nthree\r\n", f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"two\",\"new_string\":\"TWO\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    /* diff display trims the trailing \r from CRLF lines */
    ck_assert_ptr_nonnull(strstr(r->content, "-2 two\n+2 TWO"));
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "one\r\nTWO\r\nthree\r\n");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_bom_preserved)
{
    /* T3a: a UTF-8 BOM is stripped for matching and restored on write. */
    char ws[] = "/tmp/echo_rif_bom_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "wb");
    ck_assert_ptr_nonnull(f);
    fputs("\xEF\xBB\xBFhello", f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"hello\",\"new_string\":\"HELLO\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "\xEF\xBB\xBFHELLO");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_crlf_old_string_matches_lf)
{
    /* T3a: an old_string written with plain \n matches text in a CRLF
     * file (matching happens in normalized space). */
    char ws[] = "/tmp/echo_rif_crlf2_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "wb");
    ck_assert_ptr_nonnull(f);
    fputs("a\r\nb\r\nc\r\n", f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"a\\nb\",\"new_string\":\"A\\nB\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "A\r\nB\r\nc\r\n");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_oom_rollback_mid_edit_array)
{
    /* T3d fault injection: failing the Nth str_dup mid multi-edit must
     * produce an error and leave the file untouched. */
    char ws[] = "/tmp/echo_rif_oom_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "aaa bbb ccc");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    /* str_dup calls: path, then per-edit old/new (4 more for 2 edits).
     * Fail the 4th, i.e. the second edit's old_string. */
    edit_test_set_strdup_fail(4);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"edits\":["
        "{\"old_string\":\"aaa\",\"new_string\":\"1\"},"
        "{\"old_string\":\"bbb\",\"new_string\":\"2\"}]}");
    edit_test_set_strdup_fail(-1);
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "execution_error");
    tool_result_free(r);

    char *after = read_whole_file(abs_path);
    ck_assert_str_eq(after, "aaa bbb ccc");
    free(after);

    /* Normal operation still works after the injected failure. */
    ToolResult *ok = tool->execute(tool,
        "{\"path\":\"f.txt\",\"edits\":["
        "{\"old_string\":\"aaa\",\"new_string\":\"1\"},"
        "{\"old_string\":\"bbb\",\"new_string\":\"2\"}]}");
    ck_assert_ptr_nonnull(ok);
    ck_assert_ptr_null(ok->error);
    tool_result_free(ok);
    char *done = read_whole_file(abs_path);
    ck_assert_str_eq(done, "1 2 ccc");
    free(done);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_edit_missing_file_is_file_not_found)
{
    char ws[] = "/tmp/echo_rif_missing_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"nope.txt\",\"old_string\":\"a\",\"new_string\":\"b\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "file_not_found");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

START_TEST(test_edit_missing_args_is_validation_error)
{
    char ws[] = "/tmp/echo_rif_val_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"path\":\"/tmp/x\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

/* C2 regression: a short fwrite (disk full) used to be ignored — the tool
 * reported success after silently truncating the file. It must now return
 * an error and leave the file untouched. */
START_TEST(test_edit_write_failure_reports_error)
{
    char ws[] = "/tmp/echo_rif_write_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "foo bar foo");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_edit_create(safety);
    ck_assert_ptr_nonnull(tool);
    edit_test_set_fwrite_fail(1);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"bar\",\"new_string\":\"X\"}");
    edit_test_set_fwrite_fail(0);
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "execution_error");
    tool_result_free(r);

    /* file untouched */
    char *after = read_whole_file(abs_path);
    ck_assert_str_eq(after, "foo bar foo");
    free(after);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(unlink(abs_path), 0);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("ReplaceInFile");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_edit_replaces_unique_occurrence);
    tcase_add_test(tc, test_edit_no_match_is_validation_error);
    tcase_add_test(tc, test_edit_duplicate_old_string_reports_count);
    tcase_add_test(tc, test_edit_multiple_disjoint_edits);
    tcase_add_test(tc, test_edit_diff_shows_context_and_line_numbers);
    tcase_add_test(tc, test_edit_diff_context_capped_above_and_below);
    tcase_add_test(tc, test_edit_multiple_hunks_each_with_context);
    tcase_add_test(tc, test_edit_multiline_removal_shows_only_changed_lines);
    tcase_add_test(tc, test_edit_overlapping_edits_error);
    tcase_add_test(tc, test_edit_empty_old_string_error);
    tcase_add_test(tc, test_edit_noop_reports_error);
    tcase_add_test(tc, test_edit_edit_index_in_error_message);
    tcase_add_test(tc, test_edit_crlf_preserved);
    tcase_add_test(tc, test_edit_bom_preserved);
    tcase_add_test(tc, test_edit_crlf_old_string_matches_lf);
    tcase_add_test(tc, test_edit_oom_rollback_mid_edit_array);
    tcase_add_test(tc, test_edit_missing_file_is_file_not_found);
    tcase_add_test(tc, test_edit_missing_args_is_validation_error);
    tcase_add_test(tc, test_edit_write_failure_reports_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
