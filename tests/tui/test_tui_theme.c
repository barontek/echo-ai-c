/*
 * test_tui_theme.c - theme preset resolution: palette correctness, role
 * mapping, config fallback flags, monochrome mode, accent override, and
 * the mix helper. Pure logic — no terminal involved. Depends on: check.
 */

#include <check.h>

#include "tui/tui_theme.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

START_TEST(test_dark_default_colors)
{
    TuiTheme *t = tui_theme_create(NULL, NULL, NULL);
    ck_assert_ptr_nonnull(t);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_FG), RGB(0xdc, 0xd7, 0xba));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_BG), RGB(0x1e, 0x1e, 0x2e));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0x7a, 0xa2, 0xf7));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_USER), RGB(0x7a, 0xa2, 0xf7));
    ck_assert_int_eq(tui_theme_density(t), TUI_DENSITY_COMPACT);
    ck_assert_int_eq(tui_theme_fell_back(t), 0);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_light_palette)
{
    TuiTheme *t = tui_theme_create("light", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_FG), RGB(0x2d, 0x2a, 0x26));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_BG), RGB(0xf2, 0xf0, 0xe8));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0x2f, 0x5f, 0xb3));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_highcontrast_palette)
{
    TuiTheme *t = tui_theme_create("highcontrast", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_FG), RGB(0xff, 0xff, 0xff));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_BG), RGB(0x00, 0x00, 0x00));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0xff, 0xd7, 0x00));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_none_palette_is_monochrome_but_keeps_error_reverse)
{
    TuiTheme *t = tui_theme_create("none", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_FG), RGB(0xff, 0xff, 0xff));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0xff, 0xff, 0xff));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_USER), RGB(0xff, 0xff, 0xff));
    ck_assert_int_eq(tui_theme_styles(t, TUI_ROLE_THINK), 0); /* no dim/italic */
    ck_assert_int_eq(tui_theme_styles(t, TUI_ROLE_ERROR) & TUI_STYLE_REVERSE, TUI_STYLE_REVERSE);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_think_is_dim_italic_in_normal_presets)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    unsigned styles = tui_theme_styles(t, TUI_ROLE_THINK);
    ck_assert_int_eq(styles & TUI_STYLE_DIM, TUI_STYLE_DIM);
    ck_assert_int_eq(styles & TUI_STYLE_ITALIC, TUI_STYLE_ITALIC);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_think_color_differs_from_reply)
{
    /* <think> text must never blend into the assistant reply: it gets a
     * muted color of its own in the dark and light presets ... */
    TuiTheme *dark = tui_theme_create("dark", NULL, NULL);
    ck_assert_int_ne(tui_theme_color(dark, TUI_ROLE_THINK),
                     tui_theme_color(dark, TUI_ROLE_BASE_FG));
    ck_assert_int_eq(tui_theme_color(dark, TUI_ROLE_THINK),
                     RGB(0x9a, 0xa5, 0xce));
    tui_theme_free(dark);

    TuiTheme *light = tui_theme_create("light", NULL, NULL);
    ck_assert_int_ne(tui_theme_color(light, TUI_ROLE_THINK),
                     tui_theme_color(light, TUI_ROLE_BASE_FG));
    ck_assert_int_eq(tui_theme_color(light, TUI_ROLE_THINK),
                     RGB(0x4c, 0x56, 0x6a));
    tui_theme_free(light);

    /* ... while highcontrast and none stay white for legibility */
    TuiTheme *high = tui_theme_create("highcontrast", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(high, TUI_ROLE_THINK), RGB(0xff, 0xff, 0xff));
    tui_theme_free(high);

    TuiTheme *none = tui_theme_create("none", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(none, TUI_ROLE_THINK), RGB(0xff, 0xff, 0xff));
    tui_theme_free(none);
}
END_TEST

START_TEST(test_status_bar_is_inverted_accent)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_STATUS_BG), RGB(0x7a, 0xa2, 0xf7));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_STATUS_FG), RGB(0x1e, 0x1e, 0x2e));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_invalid_style_falls_back_and_flags)
{
    TuiTheme *t = tui_theme_create("neon-pink", NULL, NULL);
    ck_assert_ptr_nonnull(t);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_BG), RGB(0x1e, 0x1e, 0x2e));
    ck_assert_int_eq(tui_theme_fell_back(t), 1);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_style_names_case_insensitive)
{
    TuiTheme *t = tui_theme_create("LIGHT", "SPACIOUS", NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BASE_BG), RGB(0xf2, 0xf0, 0xe8));
    ck_assert_int_eq(tui_theme_density(t), TUI_DENSITY_SPACIOUS);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_invalid_density_flags)
{
    TuiTheme *t = tui_theme_create("dark", "huge", NULL);
    ck_assert_int_eq(tui_theme_density(t), TUI_DENSITY_COMPACT);
    ck_assert_int_eq(tui_theme_fell_back(t), 1);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_accent_hex_override)
{
    TuiTheme *t = tui_theme_create("dark", NULL, "ff0000");
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0xff, 0x00, 0x00));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_USER), RGB(0xff, 0x00, 0x00));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_STATUS_BG), RGB(0xff, 0x00, 0x00));
    ck_assert_int_eq(tui_theme_fell_back(t), 0);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_accent_hex_with_hash_and_uppercase)
{
    TuiTheme *t = tui_theme_create("dark", NULL, "#AABBCC");
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0xaa, 0xbb, 0xcc));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_invalid_accent_falls_back_and_flags)
{
    TuiTheme *t = tui_theme_create("dark", NULL, "zzz999");
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0x7a, 0xa2, 0xf7));
    ck_assert_int_eq(tui_theme_fell_back(t), 1);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_accent_ignored_in_none_mode)
{
    TuiTheme *t = tui_theme_create("none", NULL, "ff0000");
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_ACCENT), RGB(0xff, 0xff, 0xff));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_mix_helper_bounds)
{
    ck_assert_int_eq(tui_theme_mix(RGB(0x00, 0x00, 0x00), RGB(0xff, 0xff, 0xff), 0),
                     RGB(0x00, 0x00, 0x00));
    ck_assert_int_eq(tui_theme_mix(RGB(0x00, 0x00, 0x00), RGB(0xff, 0xff, 0xff), 100),
                     RGB(0xff, 0xff, 0xff));
    /* clamping: below 0 and above 100 behave like 0 / 100 */
    ck_assert_int_eq(tui_theme_mix(RGB(0x00, 0x00, 0x00), RGB(0xff, 0xff, 0xff), -5),
                     RGB(0x00, 0x00, 0x00));
    ck_assert_int_eq(tui_theme_mix(RGB(0x00, 0x00, 0x00), RGB(0xff, 0xff, 0xff), 150),
                     RGB(0xff, 0xff, 0xff));
    /* 50% blend of pure black and pure white is middle gray, per channel */
    ck_assert_int_eq(tui_theme_mix(RGB(0x00, 0x00, 0x00), RGB(0xff, 0xff, 0xff), 50),
                     RGB(0x7f, 0x7f, 0x7f));
}
END_TEST

START_TEST(test_tool_result_and_backdrop_roles)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_TOOL_RESULT), RGB(0x9e, 0xce, 0x6a));
    /* backdrop = base_bg blended 50% toward black */
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BACKDROP),
                     tui_theme_mix(RGB(0x1e, 0x1e, 0x2e), 0x000000, 50));
    tui_theme_free(t);

    t = tui_theme_create("light", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_TOOL_RESULT), RGB(0x3f, 0x62, 0x12));
    tui_theme_free(t);

    /* monochrome: tool result is plain foreground, backdrop is black */
    t = tui_theme_create("none", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_TOOL_RESULT), RGB(0xff, 0xff, 0xff));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_BACKDROP), RGB(0x00, 0x00, 0x00));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_border_is_muted_foreground)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    uint32_t border = tui_theme_color(t, TUI_ROLE_BORDER);
    ck_assert_int_eq(border, tui_theme_mix(RGB(0xdc, 0xd7, 0xba), RGB(0x1e, 0x1e, 0x2e), 50));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_spinner_frames_available)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    size_t count = 0;
    const char *const *frames = tui_theme_spinner_frames(t, &count);
    ck_assert_ptr_nonnull(frames);
    ck_assert_int_ge(count, 1);
    ck_assert_str_eq(frames[0], "⠋");
    tui_theme_free(t);
}
END_TEST

START_TEST(test_plane_colors_error_role_reverses_to_opaque_red)
{
    /* The error highlight is a design element: in transparent mode it
     * still paints an opaque background (REVERSE swaps fg/bg). */
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    uint32_t fg = 0;
    uint32_t bg = 0;
    int opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_ERROR, &fg, &bg);
    ck_assert_int_eq(opaque, 1);
    ck_assert_int_eq(fg, RGB(0x1e, 0x1e, 0x2e)); /* base_bg */
    ck_assert_int_eq(bg, RGB(0xf2, 0x6d, 0x6d)); /* error red */
    tui_theme_free(t);
}
END_TEST

START_TEST(test_plane_colors_transparent_mode_resets_bg_for_plain_roles)
{
    /* Regression (2026-08-14): the approval bar and error blocks painted
     * opaque backgrounds that leaked into every cell written afterwards
     * in transparent mode, because plain roles never reset the plane's
     * background channel — cancelling an approval left the whole chat
     * pane red. Plain roles must resolve to a transparent background so
     * the renderer resets the plane instead of reusing a stale one. */
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    uint32_t fg = 0;
    uint32_t bg = 0;
    int opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_BASE_FG, &fg, &bg);
    ck_assert_int_eq(opaque, 0);
    ck_assert_int_eq(fg, RGB(0xdc, 0xd7, 0xba));
    opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_USER, &fg, &bg);
    ck_assert_int_eq(opaque, 0);
    opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_ASSISTANT, &fg, &bg);
    ck_assert_int_eq(opaque, 0);
    opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_TOOL_RESULT, &fg, &bg);
    ck_assert_int_eq(opaque, 0);
    opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_BORDER, &fg, &bg);
    ck_assert_int_eq(opaque, 0);
    tui_theme_free(t);
}
END_TEST

START_TEST(test_plane_colors_status_fg_keeps_opaque_accent_bg)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    uint32_t fg = 0;
    uint32_t bg = 0;
    int opaque = tui_theme_plane_colors(t, 1, TUI_ROLE_STATUS_FG, &fg, &bg);
    ck_assert_int_eq(opaque, 1);
    ck_assert_int_eq(fg, RGB(0x1e, 0x1e, 0x2e)); /* base_bg on the bar */
    ck_assert_int_eq(bg, RGB(0x7a, 0xa2, 0xf7)); /* accent field */
    tui_theme_free(t);
}
END_TEST

START_TEST(test_plane_colors_opaque_mode_always_paints_bg)
{
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    uint32_t fg = 0;
    uint32_t bg = 0;
    int opaque = tui_theme_plane_colors(t, 0, TUI_ROLE_BASE_FG, &fg, &bg);
    ck_assert_int_eq(opaque, 1);
    ck_assert_int_eq(bg, RGB(0x1e, 0x1e, 0x2e));
    opaque = tui_theme_plane_colors(t, 0, TUI_ROLE_ASSISTANT, &fg, &bg);
    ck_assert_int_eq(opaque, 1);
    opaque = tui_theme_plane_colors(t, 0, TUI_ROLE_ERROR, &fg, &bg);
    ck_assert_int_eq(opaque, 1);
    ck_assert_int_eq(bg, RGB(0xf2, 0x6d, 0x6d));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_plane_colors_think_dim_mixes_fg)
{
    /* DIM styles blend the foreground toward the background; the plain
     * base background applies before any swap. */
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    uint32_t fg = 0;
    uint32_t bg = 0;
    int opaque = tui_theme_plane_colors(t, 0, TUI_ROLE_THINK, &fg, &bg);
    ck_assert_int_eq(opaque, 1);
    ck_assert_int_eq(bg, RGB(0x1e, 0x1e, 0x2e));
    ck_assert_int_eq(fg, tui_theme_mix(RGB(0x9a, 0xa5, 0xce), RGB(0x1e, 0x1e, 0x2e), 60));
    tui_theme_free(t);
}
END_TEST

START_TEST(test_diff_line_classifier)
{
    /* the edit tool's diff shape: +/- with line numbers, context with
     * line numbers, and the skip marker */
    ck_assert_int_eq(tui_diff_line_kind("-90 enabled = bash, read_file", 34),
                     TUI_DIFF_DEL);
    ck_assert_int_eq(tui_diff_line_kind("+90 enabled = bash, read_file, edit", 38),
                     TUI_DIFF_ADD);
    ck_assert_int_eq(tui_diff_line_kind(" 3 l3", 6), TUI_DIFF_CONTEXT);
    ck_assert_int_eq(tui_diff_line_kind(" ...", 4), TUI_DIFF_CONTEXT);
    ck_assert_int_eq(tui_diff_line_kind("+1 foo X foo", 13), TUI_DIFF_ADD);
}
END_TEST

START_TEST(test_diff_line_classifier_rejects_plain_text)
{
    /* unrelated tool output must stay in the block role color:
     * no leading marker, no digit run, or digits without a space */
    ck_assert_int_eq(tui_diff_line_kind("Replaced 1 occurrence (9 bytes written).", 41),
                     TUI_DIFF_PLAIN);
    ck_assert_int_eq(tui_diff_line_kind("Exit code: 0", 12), TUI_DIFF_PLAIN);
    ck_assert_int_eq(tui_diff_line_kind("-1", 2), TUI_DIFF_PLAIN);
    ck_assert_int_eq(tui_diff_line_kind("+foo", 4), TUI_DIFF_PLAIN);
    ck_assert_int_eq(tui_diff_line_kind(" 123", 4), TUI_DIFF_PLAIN); /* no trailing space */
    ck_assert_int_eq(tui_diff_line_kind("", 0), TUI_DIFF_PLAIN);
    ck_assert_int_eq(tui_diff_line_kind(NULL, 0), TUI_DIFF_PLAIN);
    ck_assert_int_eq(tui_diff_line_kind("foo -3 bar", 10), TUI_DIFF_PLAIN);
}
END_TEST

START_TEST(test_diff_role_colors)
{
    /* diff roles reuse the palette's green/red; monochrome keeps white */
    TuiTheme *t = tui_theme_create("dark", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_DIFF_ADD), RGB(0x9e, 0xce, 0x6a));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_DIFF_DEL), RGB(0xf2, 0x6d, 0x6d));
    tui_theme_free(t);

    t = tui_theme_create("light", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_DIFF_ADD), RGB(0x3f, 0x62, 0x12));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_DIFF_DEL), RGB(0x8f, 0x1d, 0x1d));
    tui_theme_free(t);

    t = tui_theme_create("none", NULL, NULL);
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_DIFF_ADD), RGB(0xff, 0xff, 0xff));
    ck_assert_int_eq(tui_theme_color(t, TUI_ROLE_DIFF_DEL), RGB(0xff, 0xff, 0xff));
    tui_theme_free(t);
}
END_TEST
static Suite *suite(void)
{
    Suite *s = suite_create("tui_theme");
    TCase *tc = tcase_create("theme");
    tcase_add_test(tc, test_dark_default_colors);
    tcase_add_test(tc, test_light_palette);
    tcase_add_test(tc, test_highcontrast_palette);
    tcase_add_test(tc, test_none_palette_is_monochrome_but_keeps_error_reverse);
    tcase_add_test(tc, test_think_is_dim_italic_in_normal_presets);
    tcase_add_test(tc, test_think_color_differs_from_reply);
    tcase_add_test(tc, test_status_bar_is_inverted_accent);
    tcase_add_test(tc, test_invalid_style_falls_back_and_flags);
    tcase_add_test(tc, test_style_names_case_insensitive);
    tcase_add_test(tc, test_invalid_density_flags);
    tcase_add_test(tc, test_accent_hex_override);
    tcase_add_test(tc, test_accent_hex_with_hash_and_uppercase);
    tcase_add_test(tc, test_invalid_accent_falls_back_and_flags);
    tcase_add_test(tc, test_accent_ignored_in_none_mode);
    tcase_add_test(tc, test_mix_helper_bounds);
    tcase_add_test(tc, test_tool_result_and_backdrop_roles);
    tcase_add_test(tc, test_border_is_muted_foreground);
    tcase_add_test(tc, test_spinner_frames_available);
    tcase_add_test(tc, test_plane_colors_error_role_reverses_to_opaque_red);
    tcase_add_test(tc, test_plane_colors_transparent_mode_resets_bg_for_plain_roles);
    tcase_add_test(tc, test_plane_colors_status_fg_keeps_opaque_accent_bg);
    tcase_add_test(tc, test_plane_colors_opaque_mode_always_paints_bg);
    tcase_add_test(tc, test_plane_colors_think_dim_mixes_fg);
    tcase_add_test(tc, test_diff_line_classifier);
    tcase_add_test(tc, test_diff_line_classifier_rejects_plain_text);
    tcase_add_test(tc, test_diff_role_colors);
    suite_add_tcase(s, tc);
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
