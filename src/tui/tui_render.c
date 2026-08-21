/*
 * tui_render.c - rendering for the TUI: chat/status/tools/input/footer
 * and modal panes, drawn from the pure models (chat/input/dialogs/theme).
 * Split out of tui.c so the app shell stays small; this file holds only
 * notcurses draw calls, never agent mutation.
 * Depends on: notcurses-core, tui_internal.h, markdown.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <notcurses/notcurses.h>

#include "tui_internal.h"
#include "markdown.h"
#include "tool_args.h"

/* Cap for the compacted tool-arguments text shown on header lines. */
#define TOOL_ARGS_MAX 200

/* Display width of the first n bytes of a UTF-8 string (wcwidth-based). */
static size_t utf8_width(const char *s, size_t n)
{
    size_t i = 0;
    size_t w = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; len = 2; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; len = 3; }
        else { cp = c & 0x07; len = 4; }
        if (i + (size_t)len > n) break;
        for (int k = 1; k < len; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
        int cw = wcwidth((wchar_t)cp);
        w += cw > 0 ? (size_t)cw : 0;
        i += (size_t)len;
    }
    return w;
}

static void plane_color(TuiApp *app, struct ncplane *p, TuiRole role)
{
    uint32_t fg;
    uint32_t bg;
    int opaque = tui_theme_plane_colors(app->theme, app->transparent, role,
                                        &fg, &bg);
    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff, (fg >> 8) & 0xff, fg & 0xff);
    if (opaque)
    {
        (void)ncplane_set_bg_rgb8(p, (bg >> 16) & 0xff,
                                  (bg >> 8) & 0xff, bg & 0xff);
    }
    else
    {
        (void)ncplane_set_bg_default(p);
    }

    unsigned bits2 = tui_theme_styles(app->theme, role);
    unsigned ncstyle = 0;
    if (bits2 & TUI_STYLE_BOLD) ncstyle |= NCSTYLE_BOLD;
    if (bits2 & TUI_STYLE_ITALIC) ncstyle |= NCSTYLE_ITALIC;
    if (bits2 & TUI_STYLE_UNDERLINE) ncstyle |= NCSTYLE_UNDERLINE;
    ncplane_set_styles(p, ncstyle);
}

/* Strip <think>/</think> markers out of a segment (write into out). */
static size_t strip_markers(const char *s, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len;)
    {
        if (len - i >= 7 && strncmp(s + i, "<think>", 7) == 0) i += 7;
        else if (len - i >= 8 && strncmp(s + i, "</think>", 8) == 0) i += 8;
        else out[o++] = s[i++];
    }
    out[o] = '\0';
    return o;
}

static TuiRole kind_role(TuiBlockKind kind)
{
    switch (kind)
    {
    case TUI_BLOCK_USER: return TUI_ROLE_USER;
    case TUI_BLOCK_THINK: return TUI_ROLE_THINK;
    case TUI_BLOCK_TOOL: return TUI_ROLE_TOOL_RESULT;
    case TUI_BLOCK_ERROR: return TUI_ROLE_ERROR;
    default: return TUI_ROLE_ASSISTANT;
    }
}

/* Role label for the block header line (opencode-style message blocks). */
static const char *kind_label(TuiBlockKind kind)
{
    switch (kind)
    {
    case TUI_BLOCK_USER: return "you";
    case TUI_BLOCK_THINK: return "thinking";
    case TUI_BLOCK_TOOL: return "tool";
    case TUI_BLOCK_ERROR: return "error";
    default: return "echo";
    }
}

/* ---- markdown-aware line rendering ---- */

static int markdown_kind(TuiBlockKind kind)
{
    return kind == TUI_BLOCK_USER || kind == TUI_BLOCK_ASSISTANT ||
           kind == TUI_BLOCK_THINK;
}

/* Map MdStyle bits onto a notcurses style + fg (bg stays as the block
 * role's — see-through in transparent mode). Resets leftover styles.
 * Role-level styles (think = dim + italic) apply beneath the inline
 * markdown styles: this is the only painter for message content lines,
 * so without them a think block would render exactly like the reply. */
static void md_apply(TuiApp *app, struct ncplane *p, TuiRole role,
                     unsigned bits)
{
    uint32_t fg = tui_theme_color(app->theme, role);
    unsigned ncstyle = 0;
    unsigned role_bits = tui_theme_styles(app->theme, role);
    if (role_bits & TUI_STYLE_DIM)
        fg = tui_theme_mix(fg, tui_theme_color(app->theme, TUI_ROLE_BASE_BG), 60);
    if (role_bits & TUI_STYLE_ITALIC) ncstyle |= NCSTYLE_ITALIC;
    if (role_bits & TUI_STYLE_BOLD) ncstyle |= NCSTYLE_BOLD;
    if (bits & MD_STYLE_BOLD) ncstyle |= NCSTYLE_BOLD;
    if (bits & MD_STYLE_ITALIC) ncstyle |= NCSTYLE_ITALIC;
    if (bits & MD_STYLE_STRIKE) ncstyle |= NCSTYLE_STRUCK;
    if (bits & MD_STYLE_LINK) ncstyle |= NCSTYLE_UNDERLINE;
    if (bits & MD_STYLE_CODE)
        fg = tui_theme_color(app->theme, TUI_ROLE_TOOL_RESULT);
    if (bits & MD_STYLE_HEADING)
    {
        fg = tui_theme_color(app->theme, TUI_ROLE_ACCENT);
        ncstyle |= NCSTYLE_BOLD;
    }
    if (bits & MD_STYLE_DIM)
        fg = tui_theme_mix(fg, tui_theme_color(app->theme, TUI_ROLE_BASE_BG), 60);
    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff, (fg >> 8) & 0xff, fg & 0xff);
    (void)ncplane_set_styles(p, ncstyle);
}

/* Block header line: rail + role label. Tool blocks show the tool name
 * bold with the compact call arguments dimmed after it ("bash: ls -la");
 * without arguments they keep the "<name> tool" label. Names and args
 * are model-provided, so the whole line is clamped to the content width
 * at a codepoint boundary. */
static void put_block_header(TuiApp *app, struct ncplane *p, TuiRole role,
                             size_t cw, TuiBlockKind bk, int row,
                             const char *title, const char *args)
{
    const char *label;
    char label_buf[64];
    int show_args = 0;
    if (bk == TUI_BLOCK_TOOL && title)
    {
        /* Friendly label ("Read") over the raw tool name; the raw name
         * stays the block title so tool_end matches pending blocks. */
        label = tool_args_label(title);
        if (args && args[0] != '\0')
        {
            show_args = 1;
        }
        else
        {
            snprintf(label_buf, sizeof(label_buf), "%s tool", label); // NOLINT(cert-err33-c)
            label = label_buf;
        }
    }
    else
    {
        label = kind_label(bk);
    }

    char name_buf[64];
    (void)tui_chat_truncate_width(label, cw, name_buf, sizeof(name_buf));
    (void)ncplane_cursor_move_yx(p, row, 0);
    plane_color(app, p, role);
    (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
    (void)ncplane_cursor_move_yx(p, row, 2);
    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
    (void)ncplane_putstr(p, name_buf);
    if (show_args)
    {
        size_t used = tui_chat_display_width(name_buf, strlen(name_buf));
        if (used + 2 < cw)
        {
            char args_buf[TOOL_ARGS_MAX + 4];
            (void)tui_chat_truncate_width(args, cw - used - 2, args_buf,
                                          sizeof(args_buf));
            if (args_buf[0] != '\0')
            {
                (void)ncplane_set_styles(p, 0);
                md_apply(app, p, role, MD_STYLE_DIM);
                (void)ncplane_putstr(p, ": ");
                (void)ncplane_putstr(p, args_buf);
            }
        }
    }
    (void)ncplane_set_styles(p, 0);
}

/* Horizontal rule: a dim line of box-drawing fills the content width. */
static void put_hr(TuiApp *app, struct ncplane *p, TuiRole role,
                   char *buf, size_t bufcap, size_t cw)
{
    md_apply(app, p, role, MD_STYLE_DIM);
    size_t o = 0;
    while (o + 3 < bufcap && o < cw * 3)
    {
        buf[o++] = '\xE2';
        buf[o++] = '\x94';
        buf[o++] = '\x80';
    }
    buf[o] = '\0';
    (void)ncplane_putstr(p, buf);
}

/* One content line with markdown applied (the rail and cursor are
 * already set up; the cursor sits at col 2). Line-level kinds set a
 * whole-line style; inline runs refine it run by run. */
static void put_markdown_line(TuiApp *app, struct ncplane *p, TuiRole role,
                              const char *seg, size_t seg_len, MdLineKind lk,
                              int in_fence, MdRun *runs, size_t run_cap)
{    if (in_fence && lk != MD_LINE_CODE_FENCE)
    {
        /* fenced code is literal: nothing inside it is a delimiter */
        md_apply(app, p, role, MD_STYLE_CODE);
        (void)ncplane_putnstr(p, seg_len, seg);
        return;
    }
    unsigned line_bits = 0;
    if (lk == MD_LINE_HEADING) line_bits |= MD_STYLE_HEADING;
    else if (lk == MD_LINE_QUOTE || lk == MD_LINE_CODE_FENCE)
        line_bits |= MD_STYLE_DIM;

    /* Headings consume their whole marker (indent, hashes, one space)
     * like CommonMark does; only the heading text is drawn. */
    const char *base = seg;
    size_t base_len = seg_len;
    if (lk == MD_LINE_HEADING)
    {
        size_t skip = 0;
        while (skip < base_len && base[skip] == ' ') skip++;
        while (skip < base_len && base[skip] == '#') skip++;
        if (skip < base_len && base[skip] == ' ') skip++;
        if (skip >= base_len) return; /* bare "#": an empty heading */
        base += skip;
        base_len -= skip;
    }

    size_t n = md_parse_inline(base, base_len, runs, run_cap);
    if (n == 0)
    {
        md_apply(app, p, role, line_bits);
        (void)ncplane_putnstr(p, base_len, base);
        return;
    }
    for (size_t i = 0; i < n; i++)
    {
        size_t start = runs[i].start;
        size_t len = runs[i].len;
        if (start >= base_len) continue;
        if (start + len > base_len) len = base_len - start;
        md_apply(app, p, role, line_bits | runs[i].style);
        (void)ncplane_putnstr(p, len, base + start);
    }
    (void)ncplane_set_styles(p, 0); /* leave no style on the plane */
}

/* Diff-colored tool-result line: added lines green, removed lines red,
 * context dimmed (the classifier runs before this, so the shape is
 * known). Returns 1 when the line was drawn. */
static int put_diff_line(TuiApp *app, struct ncplane *p, TuiRole role,
                         TuiDiffKind kind, const char *seg, size_t seg_len)
{
    uint32_t fg;
    switch (kind)
    {
    case TUI_DIFF_ADD:
        fg = tui_theme_color(app->theme, TUI_ROLE_DIFF_ADD);
        break;
    case TUI_DIFF_DEL:
        fg = tui_theme_color(app->theme, TUI_ROLE_DIFF_DEL);
        break;
    case TUI_DIFF_CONTEXT:
        fg = tui_theme_mix(tui_theme_color(app->theme, role),
                           tui_theme_color(app->theme, TUI_ROLE_BASE_BG), 60);
        break;
    default:
        return 0;
    }
    (void)ncplane_set_styles(p, 0);
    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff, (fg >> 8) & 0xff,
                              fg & 0xff);
    (void)ncplane_putnstr(p, seg_len, seg);
    return 1;
}

static void render_chat(TuiApp *app)
{
    struct ncplane *p = app->chat_plane;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);

    /* Left rail column (role color) + padding: content wraps at cols-3 */
    size_t cw = cols >= 6 ? (size_t)cols - 3 : (size_t)cols;
    size_t total = tui_chat_total_lines(app->chat, cw);
    if (app->auto_scroll && total > (size_t)rows)
        app->chat_top = total - (size_t)rows;
    size_t top = app->chat_top;

    size_t virtual_line = 0;
    size_t drawn = 0;
    /* One scratch segment for the whole pass (strip_markers output is
     * never longer than its input line); marker stripping happens per
     * drawn line. */
    char *seg = malloc(cw * 4 + 1);
    char *row_buf = malloc(cw * 8 + 1);
    MdRun *runs = malloc((cw * 4 + 2) * sizeof(MdRun));
    if (!seg || !row_buf || !runs)
    {
        free(seg);
        free(row_buf);
        free(runs);
        return;
    }
    for (size_t b = 0; b < tui_chat_block_count(app->chat) && drawn < (size_t)rows; b++)
    {
        TuiBlockKind bk = tui_chat_block_kind(app->chat, b);
        TuiRole role = kind_role(bk);
        const char *text = tui_chat_block_text(app->chat, b);
        size_t lines = 0;
        const size_t *starts = tui_chat_block_line_starts(app->chat, b, cw,
                                                          &lines);

        /* Long tool blocks collapse to the threshold with a marker row
         * that the click handler toggles. Only TOOL blocks ever get the
         * marker — other kinds always render their full content. */
        int truncated = tui_chat_block_effective_collapsed(app->chat, b, cw);
        size_t content = truncated ? TUI_CHAT_COLLAPSE_THRESHOLD : lines;
        int has_marker = (bk == TUI_BLOCK_TOOL && lines > TUI_CHAT_COLLAPSE_THRESHOLD);

        /* Header line: rail + role label. Tool blocks show the tool name
         * bold with the compact call arguments dimmed ("bash: ls -la"),
         * or "<name> tool" when the call carried no arguments. */
        if (virtual_line >= top && drawn < (size_t)rows)
        {
            put_block_header(app, p, role, cw, bk, (int)drawn,
                             tui_chat_block_title(app->chat, b),
                             tui_chat_block_args(app->chat, b));
            drawn++;
        }
        virtual_line++;

        /* A pending tool block (empty text) shows the spinner animation
         * in place of content while its tool runs. */
        int pending = (bk == TUI_BLOCK_TOOL && text[0] == '\0');

        /* Markdown (headings, emphasis, tables, code fences) applies to
         * message text, not to tool results or errors. Fence state is
         * per-block and recomputed every frame: the chat model is
         * re-rendered in full, so no state survives between frames. */
        int markdown_block = markdown_kind(bk);
        int fence = 0;
        for (size_t l = 0; l < content; l++)
        {
            size_t seg_len = 0;
            MdLineKind lk = MD_LINE_PLAIN;
            MdTable *tbl = NULL;
            size_t tbl_rows = 0;
            if (starts)
            {
                size_t s = starts[l];
                size_t e = starts[l + 1];
                if (e > s)
                {
                    size_t cap = cw * 4;
                    if (e - s > cap) e = s + cap; /* pathological combining-mark runs */
                    seg_len = strip_markers(text + s, e - s, seg);
                }
                if (markdown_block && !pending)
                {
                    lk = md_line_kind(seg, seg_len);
                    if (lk == MD_LINE_CODE_FENCE)
                        fence = !fence;
                    /* A row only starts a table when the next line is a
                     * separator; a lone row renders as plain text. */
                    if (lk == MD_LINE_TABLE_ROW && !fence &&
                        l + 1 < content)
                    {
                        size_t s2 = starts[l + 1];
                        size_t e2 = starts[l + 2];
                        size_t slen = 0;
                        if (e2 > s2)
                        {
                            size_t cap2 = cw * 4;
                            if (e2 - s2 > cap2) e2 = s2 + cap2;
                            slen = strip_markers(text + s2, e2 - s2, row_buf);
                        }
                        if (md_line_kind(row_buf, slen) == MD_LINE_TABLE_SEP)
                            tbl = md_table_scan(text, starts, content, l,
                                                &tbl_rows);
                    }
                }
            }

            if (tbl)
            {
                /* Render every row (padded to the column widths), then
                 * consume the separator as a blank line; the outer loop
                 * must skip both so scroll math stays exact. */
                for (size_t r = 0; r < tbl_rows; r++)
                {
                    /* rows are non-contiguous: the separator at l + 1
                     * sits between the first row and the rest */
                    size_t rline = (r == 0) ? l : l + r + 1;
                    if (virtual_line >= top && drawn < (size_t)rows)
                    {
                        size_t rs = starts[rline];
                        size_t re = starts[rline + 1];
                        size_t rlen = 0;
                        if (re > rs)
                        {
                            size_t cap3 = cw * 4;
                            if (re - rs > cap3) re = rs + cap3;
                            rlen = strip_markers(text + rs, re - rs, seg);
                        }
                        (void)ncplane_cursor_move_yx(p, (int)drawn, 0);
                        plane_color(app, p, role);
                        (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
                        (void)ncplane_cursor_move_yx(p, (int)drawn, 2);
                        size_t n = md_table_render_row(row_buf, cw * 8 + 1,
                                                       seg, rlen, tbl);
                        (void)ncplane_putnstr(p, n, row_buf);
                        drawn++;
                    }
                    virtual_line++;
                }
                virtual_line++;
                if (virtual_line >= top && drawn < (size_t)rows)
                    drawn++;
                md_table_free(tbl);
                l += tbl_rows; /* the loop's l++ clears the separator too */
                continue;
            }

            if (virtual_line >= top && drawn < (size_t)rows)
            {
                (void)ncplane_cursor_move_yx(p, (int)drawn, 0);
                plane_color(app, p, role);
                (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
                (void)ncplane_cursor_move_yx(p, (int)drawn, 2);
                if (pending)
                {
                    size_t frames = 0;
                    const char *const *sp =
                        tui_theme_spinner_frames(app->theme, &frames);
                    if (frames > 0)
                        (void)ncplane_putnstr(p, 1,
                                              sp[app->frame % frames]);
                }
                else if (starts)
                {
                    if (markdown_block && lk == MD_LINE_HR && !fence)
                        put_hr(app, p, role, row_buf, cw * 8 + 1, cw);
                    else if (markdown_block)
                        put_markdown_line(app, p, role, seg, seg_len, lk,
                                          fence, runs, cw * 4 + 2);
                    else if (bk == TUI_BLOCK_TOOL &&
                             put_diff_line(app, p, role,
                                           tui_diff_line_kind(seg, seg_len),
                                           seg, seg_len))
                        ;
                    else
                        (void)ncplane_putnstr(p, seg_len, seg);
                }
                drawn++;
            }
            virtual_line++;
        }
        if (has_marker && virtual_line >= top && drawn < (size_t)rows)
        {
            char marker[96];
            if (truncated)
            {
                snprintf(marker, sizeof(marker), // NOLINT(cert-err33-c)
                         "  \xE2\x80\xA6 +%zu more lines \xE2\x80\x94 click to expand",
                         lines - TUI_CHAT_COLLAPSE_THRESHOLD);
            }
            else
            {
                snprintf(marker, sizeof(marker), // NOLINT(cert-err33-c)
                         "  \xE2\x80\x94 click to collapse");
            }
            (void)ncplane_cursor_move_yx(p, (int)drawn, 0);
            plane_color(app, p, role);
            (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
            (void)ncplane_cursor_move_yx(p, (int)drawn, 2);
            (void)ncplane_putnstr(p, strlen(marker), marker);
            drawn++;
        }
        virtual_line += has_marker ? 1 : 0;
        /* One separator line after every block: it must consume a screen
         * row (left blank) or blocks render adjacent — the scroll math
         * counts it, so the drawing must too. */
        virtual_line++;
        if (virtual_line >= top && drawn < (size_t)rows)
            drawn++;
    }
    free(seg);
    free(row_buf);
    free(runs);
}

static void render_status(TuiApp *app)
{
    struct ncplane *p = app->status_plane;
    unsigned cols;
    ncplane_dim_yx(p, NULL, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_STATUS_FG);

    char line[512];
    int n = snprintf(line, sizeof(line), " %s | %s | session: %s%s%s%s | tools: %d",
                     app->model ? app->model : "?",
                     app->provider ? app->provider : "?",
                     app->session_id ? app->session_id : "none",
                     app->title ? " (" : "",
                     app->title ? app->title : "",
                     app->title ? ")" : "",
                     app->ctx.tool_count);
    /* snprintf returns the would-be length on truncation: clamping keeps
     * the chained writes inside the buffer (titles come from the model
     * and are unbounded). */
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    if (tui_worker_busy(app->worker))
    {
        size_t frames = 0;
        const char *const *sp = tui_theme_spinner_frames(app->theme, &frames);
        if (frames > 0)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s",
                          sp[app->frame % frames]);
    }
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    if (app->status_msg && app->status_msg[0])
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  [%s]", app->status_msg);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    (void)ncplane_putnstr(p, (size_t)n, line);
}

static void render_tools(TuiApp *app)
{
    struct ncplane *p = app->tools_plane;
    unsigned cols;
    ncplane_dim_yx(p, NULL, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_TOOL_RESULT);

    /* Live-activity strip: shows only the currently running tool and
     * collapses when idle. Tool history lives in the chat blocks. */
    if (!app->active_tool) return;

    char line[320];
    int n;
    if (app->active_tool_args && app->active_tool_args[0])
        n = snprintf(line, sizeof(line), "%s: %s", app->active_tool,
                     app->active_tool_args);
    else
        n = snprintf(line, sizeof(line), "%s", app->active_tool);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1; /* truncated: keep chained writes in bounds */
    n += snprintf(line + n, sizeof(line) - (size_t)n,
                  "  \xE2\x80\xA6 running");
    if (tui_worker_busy(app->worker))
    {
        size_t frames = 0;
        const char *const *sp = tui_theme_spinner_frames(app->theme, &frames);
        if (frames > 0)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s",
                          sp[app->frame % frames]);
    }
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    size_t len = (size_t)n;
    if (len > (size_t)cols - 1) len = (size_t)cols - 1;
    (void)ncplane_putnstr(p, len, line);
}

static void render_input(TuiApp *app)
{
    struct ncplane *p = app->input_plane;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_BASE_FG);
    const char *text = tui_input_text(app->input);
    size_t len = strlen(text);
    size_t width = cols > 2 ? (size_t)cols - 2 : (size_t)cols;

    /* The pane grows to the wrapped line count (capped); a change marks
     * the layout stale so the next frame rebuilds the planes. */
    size_t wrapped = tui_chat_wrap(text, width, NULL, 0);
    if (wrapped == 0) wrapped = 1;
    unsigned want = wrapped > 5 ? 5U : (unsigned)wrapped;
    if (want != app->input_rows)
    {
        app->input_rows = want;
        app->term_rows = 0; /* force layout_planes on the next frame */
    }

    size_t *starts = malloc((wrapped + 1) * sizeof(size_t));
    size_t cursor_off = tui_input_cursor(app->input);
    size_t cursor_line = 0;
    size_t cursor_col = 0;
    if (starts)
    {
        (void)tui_chat_wrap(text, width, starts, wrapped + 1);
        starts[wrapped] = len;
        for (size_t l = 0; l < wrapped; l++)
        {
            if (cursor_off >= starts[l] &&
                cursor_off <= starts[l + 1])
            {
                cursor_line = l;
                cursor_col = utf8_width(text + starts[l],
                                        cursor_off - starts[l]);
                break;
            }
        }
    }
    /* Scroll the view so the cursor's line stays visible; the editor
     * cursor normally sits on the last line, i.e. show the last rows. */
    size_t first = cursor_line >= rows ? cursor_line - rows + 1 : 0;
    if (starts)
    {
        for (size_t l = first; l < wrapped && l < first + rows; l++)
        {
            size_t s = starts[l];
            size_t e = starts[l + 1];
            size_t shown = e > s ? e - s : 0;
            if (shown > width) shown = width;
            (void)ncplane_cursor_move_yx(p, (int)(l - first), 0);
            /* shell mode (leading '!') uses a '$' prompt like opencode */
            if (l == 0)
                (void)ncplane_putstr(p, tui_input_text(app->input)[0] == '!' ? "$ " : "> ");
            else (void)ncplane_putstr(p, "  ");
            (void)ncplane_putnstr(p, shown, text + s);
        }
        free(starts);
    }
    if (cursor_col > width) cursor_col = width;
    (void)ncplane_cursor_move_yx(p, (int)(cursor_line - first), 2 + (int)cursor_col);
    (void)ncplane_putnstr(p, 1, " ");
    (void)ncplane_cursor_move_yx(p, 0, 0);
}

static void render_footer(TuiApp *app)
{
    struct ncplane *p = app->footer_plane;
    unsigned cols;
    ncplane_dim_yx(p, NULL, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_BORDER);
    const char *hints;
    if (tui_keymap_leader_active(app->keymap))
        hints = " \xE2\x80\xA6 waiting for leader chord (Esc cancels) ";
    else
        hints = " Enter send | Shift+Enter newline | Esc cancel | Ctrl-P commands | Ctrl-C quit | Ctrl-X leader ";
    size_t n = strlen(hints);
    if (n > (size_t)cols - 1) n = (size_t)cols - 1;
    (void)ncplane_putnstr(p, n, hints);
}

static const char *modal_hint(TuiModalKind kind)
{
    switch (kind)
    {
    case TUI_MODAL_APPROVAL: return "h/l or 1-3 choose \xE2\x80\xA2 enter confirm \xE2\x80\xA2 esc reject";
    case TUI_MODAL_CONFIRM_QUIT: return "y quit \xE2\x80\xA2 any other key cancels";
    case TUI_MODAL_PASSWORD:
    case TUI_MODAL_ASK_USER: return "enter submit \xE2\x80\xA2 esc cancel";
    case TUI_MODAL_PICKER: return "type to filter \xE2\x80\xA2 up/down move \xE2\x80\xA2 enter select \xE2\x80\xA2 esc cancel";
    case TUI_MODAL_CONFIRM: return "y confirm \xE2\x80\xA2 n cancel";
    default: return "any key to dismiss";
    }
}

/* Bracket-free modal panel: full-width accent title bar, wrapped body,
 * input row, and an action-hint line. The backdrop dims the rest. */
/* Approval prompt text: " allow <tool>?  <args>   — y allow • n deny". */
static char *approval_prompt_text_alloc(const TuiApp *app)
{
    const char *title = tui_modal_title(app->modal);
    const char *body = tui_modal_body(app->modal);
    if (!title) title = "";
    if (!body) body = "";
    int sel = app->modal && tui_modal_kind(app->modal) == TUI_MODAL_APPROVAL
                  ? app->modal->selection : 0;
    if (sel < 0 || sel > 2) sel = 0;
    static const char *opts[3] = { "Allow once", "Always", "Reject" };
    char hint[96];
    snprintf(hint, sizeof(hint), // NOLINT(cert-err33-c)
             "   \xE2\x80\x94 %s%s  \xE2\x80\xA2 %s%s  \xE2\x80\xA2 %s%s  (h/l or 1-3, Enter confirm, Esc reject)",
             sel == 0 ? "\xE2\x96\xB6 " : "  ", opts[0],
             sel == 1 ? "\xE2\x96\xB6 " : "  ", opts[1],
             sel == 2 ? "\xE2\x96\xB6 " : "  ", opts[2]);
    size_t n = strlen(title) + strlen(body) + strlen(hint) + 16;
    char *out = malloc(n);
    if (!out) return NULL;
    int k = snprintf(out, n, " allow %s?  %s%s", title, body, hint);
    if (k < 0 || (size_t)k >= n)
    {
        free(out);
        return NULL;
    }
    return out;
}

/* Wrapped height of the approval prompt bar at the current width; 0 when
 * no approval bar is up. Allocation failure falls back to one line so a
 * modal still renders. */
int approval_bar_lines(const TuiApp *app, unsigned cols)
{
    if (!app->modal || tui_modal_kind(app->modal) != TUI_MODAL_APPROVAL)
        return 0;
    char *text = approval_prompt_text_alloc(app);
    if (!text) return 1;
    size_t width = cols >= 2 ? (size_t)cols - 1 : (size_t)cols;
    size_t lines = tui_chat_wrap(text, width, NULL, 0);
    free(text);
    return lines < 1 ? 1 : (int)lines;
}

/* Approval prompt bar: accent-background lines just above the input box —
 * no backdrop, no screen takeover. Fits on one line when the terminal is
 * wide enough, wraps across multiple lines otherwise so the full request
 * stays readable. */
static void render_prompt_bar(TuiApp *app)
{
    if (!app->prompt_plane || !app->modal) return;
    struct ncplane *p = app->prompt_plane;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_STATUS_FG);

    char *text = approval_prompt_text_alloc(app);
    if (!text) return;
    size_t width = cols >= 2 ? (size_t)cols - 1 : (size_t)cols;
    size_t blines = tui_chat_wrap(text, width, NULL, 0);
    size_t *starts = malloc((blines + 1) * sizeof(size_t));
    if (starts)
    {
        (void)tui_chat_wrap(text, width, starts, blines + 1);
        starts[blines] = strlen(text);
        size_t shown = blines < rows ? blines : rows;
        for (size_t l = 0; l < shown; l++)
        {
            size_t s = starts[l];
            size_t e = starts[l + 1];
            if (e <= s) continue;
            (void)ncplane_cursor_move_yx(p, (int)l, 0);
            (void)ncplane_putnstr(p, e - s, text + s);
        }
        free(starts);
    }
    free(text);
}

static void render_modal(TuiApp *app)
{
    if (!app->modal)
    {
        /* Every close path funnels through here: planes left behind after
         * the modal struct is gone would sit on top of the z-buffer
         * forever, freezing the previous dialog on screen. */
        if (app->modal_plane)
        {
            ncplane_destroy(app->modal_plane);
            app->modal_plane = NULL;
        }
        if (app->backdrop_plane)
        {
            ncplane_destroy(app->backdrop_plane);
            app->backdrop_plane = NULL;
        }
        return;
    }
    /* Approvals render as the bottom prompt bar, not a full-screen modal */
    if (tui_modal_kind(app->modal) == TUI_MODAL_APPROVAL)
        return;
    unsigned rows, cols;
    notcurses_term_dim_yx(app->nc, &rows, &cols);
    int want = tui_modal_kind(app->modal) == TUI_MODAL_PICKER
                   ? tui_modal_picker_item_count(app->modal) + 4 : 8;
    int mh = want;
    if (mh < 8) mh = 8;
    if (mh > 20) mh = 20; /* picker lists cap the window */
    if ((int)rows < mh + 2) mh = (int)rows - 2;
    int my = ((int)rows - mh) / 2;

    /* Full-screen dim layer so content behind the modal is obscured */
    if (!app->backdrop_plane)
    {
        app->backdrop_plane = make_plane(app, rows, cols, 0, 0);
        if (app->backdrop_plane)
        {
            uint32_t bg = tui_theme_color(app->theme, TUI_ROLE_BACKDROP);
            (void)ncplane_set_bg_rgb8(app->backdrop_plane,
                                      (bg >> 16) & 0xff, (bg >> 8) & 0xff, bg & 0xff);
            (void)ncplane_set_base(app->backdrop_plane, " ", 0, 0);
            (void)ncplane_erase(app->backdrop_plane);
        }
    }
    if (!app->modal_plane)
        app->modal_plane = make_plane(app, (unsigned)mh, cols, my, 0);
    struct ncplane *p = app->modal_plane;
    ncplane_erase(p);

    /* Title bar: accent background, dark text. In transparent mode the
     * modal blends with the terminal: accent text, no background. */
    if (app->transparent)
    {
        uint32_t accent = tui_theme_color(app->theme, TUI_ROLE_ACCENT);
        (void)ncplane_set_fg_rgb8(p, (accent >> 16) & 0xff,
                                  (accent >> 8) & 0xff, accent & 0xff);
    }
    else
    {
        plane_color(app, p, TUI_ROLE_STATUS_FG);
    }
    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
    const char *title = tui_modal_title(app->modal);
    size_t tn = strlen(title);
    if (tn > (size_t)cols - 2) tn = (size_t)cols - 2;
    (void)ncplane_putnstr(p, tn, title);
    (void)ncplane_set_styles(p, 0);

    /* Picker list: type-to-filter rows with a highlighted cursor. */
    if (tui_modal_kind(app->modal) == TUI_MODAL_PICKER)
    {
        int list_rows = mh - 3;
        if (list_rows < 1) list_rows = 1;
        tui_modal_picker_set_window(app->modal, list_rows);
        int top = tui_modal_picker_top(app->modal);
        int visible = tui_modal_picker_visible_count(app->modal);
        int cursor = tui_modal_picker_cursor(app->modal);
        if (visible == 0)
        {
            plane_color(app, p, TUI_ROLE_BORDER);
            (void)ncplane_cursor_move_yx(p, 1, 2);
            (void)ncplane_putstr(p, "(no matches)");
        }
        else
        {
            for (int l = 0; l < list_rows && top + l < visible; l++)
            {
                int row = top + l;
                const char *item = tui_modal_picker_visible_at(app->modal, row);
                if (tui_modal_picker_visible_is_header(app->modal, row))
                {
                    /* category title: muted boxed divider, never highlighted */
                    plane_color(app, p, TUI_ROLE_BORDER);
                    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
                    size_t ilen = item ? strlen(item) : 0;
                    if (ilen > (size_t)cols - 6) ilen = (size_t)cols - 6;
                    char hdr[80];
                    if (ilen + 6 < sizeof(hdr))
                    {
                        snprintf(hdr, sizeof(hdr), "\xE2\x94\x80 %.48s \xE2\x94\x80", // NOLINT(cert-err33-c)
                                 item ? item : "");
                        ilen = strlen(hdr);
                    }
                    else if (item)
                    {
                        snprintf(hdr, sizeof(hdr), "%.*s", (int)(cols - 4), item); // NOLINT(cert-err33-c)
                        ilen = strlen(hdr);
                    }
                    else
                    {
                        hdr[0] = '\0';
                    }
                    if (ilen > (size_t)cols - 4) ilen = (size_t)cols - 4;
                    (void)ncplane_cursor_move_yx(p, 1 + l, 2);
                    (void)ncplane_putnstr(p, ilen, hdr);
                    (void)ncplane_set_styles(p, 0);
                    continue;
                }
                if (row == cursor)
                {
                    uint32_t bg = tui_theme_color(app->theme, TUI_ROLE_STATUS_BG);
                    (void)ncplane_set_bg_rgb8(p, (bg >> 16) & 0xff,
                                              (bg >> 8) & 0xff, bg & 0xff);
                    uint32_t fg = tui_theme_color(app->theme, TUI_ROLE_STATUS_FG);
                    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff,
                                              (fg >> 8) & 0xff, fg & 0xff);
                    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
                }
                else
                {
                    plane_color(app, p, TUI_ROLE_BASE_FG);
                }
                if (item)
                {
                    size_t ilen = strlen(item);
                    if (ilen > (size_t)cols - 4) ilen = (size_t)cols - 4;
                    (void)ncplane_cursor_move_yx(p, 1 + l, 2);
                    (void)ncplane_putnstr(p, ilen, item);
                }
            }
        }
        /* Filter line (the picker reuses the input row for type-to-filter) */
        plane_color(app, p, TUI_ROLE_BASE_FG);
        (void)ncplane_cursor_move_yx(p, mh - 2, 2);
        (void)ncplane_putstr(p, "> ");
        const char *in = tui_modal_text(app->modal);
        size_t max = (size_t)cols - 4;
        size_t inlen = strlen(in);
        if (inlen > max) inlen = max;
        (void)ncplane_putnstr(p, inlen, in);

        plane_color(app, p, TUI_ROLE_BORDER);
        (void)ncplane_cursor_move_yx(p, mh - 1, 2);
        const char *phint = modal_hint(TUI_MODAL_PICKER);
        size_t phn = strlen(phint);
        if (phn > (size_t)cols - 4) phn = (size_t)cols - 4;
        (void)ncplane_putnstr(p, phn, phint);
        return;
    }

    /* Body: wrapped across the middle rows */
    plane_color(app, p, TUI_ROLE_BASE_FG);
    const char *body = tui_modal_body(app->modal);
    if (body)
    {
        int body_rows = mh - 3;
        if (body_rows < 1) body_rows = 1;
        size_t bw = cols >= 4 ? (size_t)cols - 4 : (size_t)cols;
        size_t blines = tui_chat_wrap(body, bw, NULL, 0);
        size_t *starts = malloc((blines + 1) * sizeof(size_t));
        if (starts)
        {
            (void)tui_chat_wrap(body, bw, starts, blines + 1);
            starts[blines] = strlen(body);
            size_t shown = blines < (size_t)body_rows ? blines : (size_t)body_rows;
            for (size_t l = 0; l < shown; l++)
            {
                size_t s = starts[l];
                size_t e = starts[l + 1];
                if (e <= s) continue;
                (void)ncplane_cursor_move_yx(p, 1 + (int)l, 2);
                (void)ncplane_putnstr(p, e - s, body + s);
            }
            if (blines > shown)
            {
                (void)ncplane_cursor_move_yx(p, 1 + (int)shown - 1, 2);
                (void)ncplane_putstr(p, "\xE2\x80\xA6"); /* … */
            }
            free(starts);
        }
    }

    /* Input row (password / ask_user) */
    if (tui_modal_needs_input(tui_modal_kind(app->modal)))
    {
        (void)ncplane_cursor_move_yx(p, mh - 2, 2);
        (void)ncplane_putstr(p, "> ");
        const char *in = tui_modal_text(app->modal);
        char masked[128];
        size_t max = (size_t)cols - 4;
        if (tui_modal_kind(app->modal) == TUI_MODAL_PASSWORD)
        {
            tui_modal_mask(in, masked, sizeof(masked));
            size_t mn = strlen(masked);
            if (mn > max) mn = max;
            (void)ncplane_putnstr(p, mn, masked);
        }
        else
        {
            size_t inlen = strlen(in);
            if (inlen > max) inlen = max;
            (void)ncplane_putnstr(p, inlen, in);
        }
    }

    /* Hint line */
    plane_color(app, p, TUI_ROLE_BORDER);
    (void)ncplane_cursor_move_yx(p, mh - 1, 2);
    const char *hint = modal_hint(tui_modal_kind(app->modal));
    size_t hn = strlen(hint);
    if (hn > (size_t)cols - 4) hn = (size_t)cols - 4;
    (void)ncplane_putnstr(p, hn, hint);
}

/* Right-hand sidebar: session inventory. echo-ai exposes no MCP/LSP/
 * todo/context-token sources (opencode concepts), so this panel shows the
 * session facts that exist and labels the rest not-applicable. */
static void render_sidebar(TuiApp *app)
{
    struct ncplane *p = app->sidebar_plane;
    if (!p) return;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_BASE_FG);
    (void)ncplane_set_bg_default(p);
    if (cols < 4 || rows < 4) return;

    (void)ncplane_cursor_move_yx(p, 0, 0);
    (void)ncplane_putstr(p, " \xE2\x94\x80\xE2\x94\x80 Session \xE2\x94\x80\xE2\x94\x80 ");
    int y = 1;
    char line[128];
    snprintf(line, sizeof(line), " id:    %.12s", // NOLINT(cert-err33-c)
             app->session_id ? app->session_id : "(none)");
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, line);
    snprintf(line, sizeof(line), " model:  %s", app->model ? app->model : "(none)"); // NOLINT(cert-err33-c)
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, line);
    snprintf(line, sizeof(line), " prov:   %s", app->provider ? app->provider : "(none)"); // NOLINT(cert-err33-c)
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, line);
    snprintf(line, sizeof(line), " tools:  %d", registry_enabled_count()); // NOLINT(cert-err33-c)
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, line);
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, " \xE2\x94\x80\xE2\x94\x80 Not available \xE2\x94\x80\xE2\x94\x80 ");
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, " MCP, LSP, todo and");
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, " context-token panels");
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, " need data sources");
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, " echo-ai does not have.");
    (void)ncplane_cursor_move_yx(p, y++, 0);
    (void)ncplane_putstr(p, " Toggle with <leader>b.");
}

void render_all(TuiApp *app)
{
    render_status(app);
    render_tools(app);
    render_chat(app);
    if (app->sidebar_visible)
        render_sidebar(app);
    render_input(app);
    render_footer(app);
    render_prompt_bar(app);
    render_modal(app);
    (void)notcurses_render(app->nc);
}

/* ---- event dispatch ---- */
