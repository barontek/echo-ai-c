/*
 * tui_chat.h - chat scrollback model: message blocks (user/assistant/
 * think/tool/error), a single open streaming block, width-aware word
 * wrapping, and viewport clamping. No terminal I/O — the renderer walks
 * tui_chat_wrap() output. Depends on: stdlib.
 */

#ifndef ECHO_TUI_CHAT_H
#define ECHO_TUI_CHAT_H

#include <stddef.h>

typedef enum {
    TUI_BLOCK_USER,
    TUI_BLOCK_ASSISTANT,
    TUI_BLOCK_THINK,
    TUI_BLOCK_TOOL,
    TUI_BLOCK_ERROR
} TuiBlockKind;

/* Long tool blocks auto-collapse to this many visible lines; the row
 * after them is a click-to-toggle marker. */
#define TUI_CHAT_COLLAPSE_THRESHOLD 8

/* Per-block collapse state. AUTO collapses long tool blocks until the
 * user toggles them; ON/OFF are the user's explicit choices. */
typedef enum {
    TUI_COLLAPSE_AUTO = 0,
    TUI_COLLAPSE_ON = 1,
    TUI_COLLAPSE_OFF = 2
} TuiCollapseState;

typedef struct TuiChat TuiChat;

/*
 * Wrap caching contract: the block accessors below (render lines, line
 * starts, collapse state) may compute and cache wrap offsets inside the
 * chat, invalidated automatically when the block's text changes or the
 * requested width differs. They therefore require a mutable chat and are
 * single-threaded (the UI thread), like every other chat operation.
 */

/**
 * tui_chat_create - allocate an empty scrollback
 *
 * Return: caller-owned TuiChat, or NULL on allocation failure. Release
 *   with tui_chat_destroy().
 */
TuiChat *tui_chat_create(void);

/**
 * tui_chat_destroy - release a scrollback and all blocks
 * @chat: scrollback to release, or NULL (no-op).
 *
 * Return: void.
 */
void tui_chat_destroy(TuiChat *chat);

/**
 * tui_chat_begin_user - seal any open streaming block and append a user block
 * @chat: scrollback; non-NULL.
 * @text: user message, borrowed.
 *
 * Commit rule: if a streaming block is open it is sealed first (no
 * allocation involved). On allocation failure the scrollback is exactly as
 * before the call — the streaming block stays open — and -1 is returned.
 *
 * Return: 0 on success, -1 on allocation failure (state unchanged).
 */
int tui_chat_begin_user(TuiChat *chat, const char *text);

/**
 * tui_chat_begin_stream - seal any open streaming block, open a new one
 * @chat: scrollback; non-NULL.
 * @kind: TUI_BLOCK_ASSISTANT or TUI_BLOCK_THINK (other kinds rejected).
 *
 * The new block starts empty and accumulates via tui_chat_stream_append.
 * Same failure contract as tui_chat_begin_user.
 *
 * Return: 0 on success, -1 on allocation failure or invalid kind.
 */
int tui_chat_begin_stream(TuiChat *chat, TuiBlockKind kind);

/**
 * tui_chat_stream_append - append a delta to the open streaming block
 * @chat: scrollback; non-NULL.
 * @delta: bytes to append, borrowed.
 *
 * On allocation failure the block text is unchanged (no partial append)
 * and -1 is returned. No-op success when there is no open streaming block.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_chat_stream_append(TuiChat *chat, const char *delta);

/**
 * tui_chat_end_stream - seal the open streaming block
 * @chat: scrollback; non-NULL.
 *
 * Never fails (no allocation).
 *
 * Return: void.
 */
void tui_chat_end_stream(TuiChat *chat);

/**
 * tui_chat_append_tool - append a tool-activity block
 * @chat: scrollback; non-NULL.
 * @name: tool name, borrowed.
 * @result: tool result or error text, borrowed.
 *
 * The block text is "<name>: <result>". Failure contract as above.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_chat_append_tool(TuiChat *chat, const char *name, const char *result);

/**
 * tui_chat_begin_tool - open a pending tool block for a running tool
 * @chat: scrollback; non-NULL.
 * @name: tool name, borrowed (kept as the block's header title).
 * @args: compact one-line tool arguments (see tool_args_compact), or
 *   NULL when the call carried none; borrowed (kept as the block's
 *   header args).
 *
 * Appends a TOOL block with empty text, the given title and args. While
 * the text stays empty the renderer shows an animation instead of
 * content; tui_chat_tool_finish() fills the same block with the result,
 * so one tool call never appears twice. Same failure contract as the
 * other appends: on allocation failure the scrollback is unchanged.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_chat_begin_tool(TuiChat *chat, const char *name, const char *args);

/**
 * tui_chat_tool_finish - fill the pending tool block with its result
 * @chat: scrollback; non-NULL.
 * @name: tool name (used to append a fallback block when no pending
 *   tool block exists, e.g. a missed start event); borrowed.
 * @result: result or error text, borrowed.
 *
 * Fills the last TOOL block when it is still pending (empty text);
 * otherwise appends "<name>: <result>" like tui_chat_append_tool.
 * On allocation failure the pending block stays pending.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_chat_tool_finish(TuiChat *chat, const char *name, const char *result);

/**
 * tui_chat_append_error - append an error block
 * @chat: scrollback; non-NULL.
 * @text: error text, borrowed.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_chat_append_error(TuiChat *chat, const char *text);

/**
 * tui_chat_block_count - number of blocks (open streaming block included)
 * @chat: scrollback; non-NULL.
 *
 * Return: block count.
 */
size_t tui_chat_block_count(const TuiChat *chat);

/**
 * tui_chat_block_kind / tui_chat_block_text - access a block
 * @chat: scrollback; non-NULL.
 * @idx: block index; out of range yields TUI_BLOCK_ERROR / "".
 *
 * Return: kind, or borrowed NUL-terminated text (never NULL).
 */
TuiBlockKind tui_chat_block_kind(const TuiChat *chat, size_t idx);
const char *tui_chat_block_text(const TuiChat *chat, size_t idx);

/**
 * tui_chat_block_title - the block's header title (tool blocks)
 * @chat: scrollback; non-NULL.
 * @idx: block index.
 *
 * Tool blocks carry the tool name as their title; the renderer composes
 * "<title> tool" for the header line.
 *
 * Return: borrowed title, or NULL when the block has none.
 */
const char *tui_chat_block_title(const TuiChat *chat, size_t idx);

/**
 * tui_chat_block_args - the block's header args (tool blocks)
 * @chat: scrollback; non-NULL.
 * @idx: block index.
 *
 * Tool blocks opened via tui_chat_begin_tool may carry a compact one-
 * line rendering of the call's arguments; the renderer shows them on
 * the header line after the title.
 *
 * Return: borrowed compact args (never NULL; "" when none were given).
 */
const char *tui_chat_block_args(const TuiChat *chat, size_t idx);

/**
 * tui_chat_block_effective_collapsed - is the block rendered truncated?
 * @chat: scrollback; non-NULL.
 * @idx: block index.
 * @width: line width in columns; must be >= 1.
 *
 * Only TOOL blocks longer than TUI_CHAT_COLLAPSE_THRESHOLD lines can be
 * truncated: AUTO and ON collapse them, OFF keeps them fully expanded.
 * Shorter blocks are never truncated regardless of state.
 *
 * Return: 1 when the renderer draws the block truncated.
 */
int tui_chat_block_effective_collapsed(TuiChat *chat, size_t idx,
                                       size_t width);

/**
 * tui_chat_block_render_lines - lines the renderer draws for a block
 * @chat: scrollback; non-NULL.
 * @idx: block index.
 * @width: line width in columns; must be >= 1.
 *
 * Every block starts with a role-label header line. Long tool blocks get
 * a marker row after their content: 1+THRESHOLD+1 lines when collapsed,
 * 1+lines+1 when expanded. Other blocks return 1 + their wrapped count.
 *
 * Return: visible line count (header and marker included).
 */
size_t tui_chat_block_render_lines(TuiChat *chat, size_t idx, size_t width);

/**
 * tui_chat_block_line_starts - cached wrap offsets for one block
 * @chat: scrollback; non-NULL.
 * @idx: block index.
 * @width: line width in columns; must be >= 1.
 * @lines: out-param receiving the wrapped line count (never NULL).
 *
 * Returns the byte offset of each wrapped line's start (line 0 starts at
 * offset 0, and one extra entry holds the text length), cached inside the
 * block: repeated calls at the same width reuse the array, and any text
 * change or width change recomputes it. On allocation failure falls back
 * to returning the line count only (starts = NULL); rendering must then
 * skip the content rows but keep the line math.
 *
 * Return: borrowed offsets array (owned by the chat, valid until the
 *   block's text changes or a different width is queried), or NULL on
 *   allocation failure.
 */
const size_t *tui_chat_block_line_starts(TuiChat *chat, size_t idx,
                                         size_t width, size_t *lines);

/**
 * tui_chat_toggle_collapse - flip a tool block between expanded/collapsed
 * @chat: scrollback; non-NULL.
 * @idx: block index.
 * @width: line width in columns; must be >= 1.
 *
 * The first toggle makes the user's choice explicit (AUTO becomes ON or
 * OFF, whichever the current rendering is not). Non-tool blocks and
 * short blocks are unchanged.
 *
 * Return: the new TuiCollapseState (AUTO for blocks that cannot be
 *   toggled).
 */
int tui_chat_toggle_collapse(TuiChat *chat, size_t idx, size_t width);

/**
 * tui_chat_wrap - greedy word wrap of one text into line-start offsets
 * @text: NUL-terminated UTF-8 input; non-NULL.
 * @width: line width in display columns; must be >= 1 (a lone codepoint
 *   wider than the line is placed anyway, overflowing the line).
 * @line_starts: caller-owned array receiving byte offsets of each line
 *   start; line 0 always starts at offset 0. May be NULL when @cap is 0.
 * @cap: capacity of @line_starts.
 *
 * Column widths come from a built-in, locale-independent table (CJK,
 * Hangul, emoji blocks are 2 columns; combining marks and variation
 * selectors are 0; wcwidth(3) is unusable here because it returns -1 in
 * the C locale). Words longer than @width break mid-word at codepoint
 * boundaries (no overflow). '\n' is a hard break. Runs of spaces collapse
 * at line starts; spaces otherwise count toward the line width.
 *
 * Return: total number of lines. When the return exceeds @cap, only the
 *   first @cap offsets were written and the caller should allocate
 *   exactly the returned count and call again.
 */
size_t tui_chat_wrap(const char *text, size_t width,
                     size_t *line_starts, size_t cap);

/**
 * tui_chat_display_width - display columns of a UTF-8 byte range
 * @s: NUL-terminated UTF-8 input; non-NULL.
 * @len: byte length to measure (may stop before the NUL).
 *
 * Uses the same locale-independent per-codepoint width table as
 * tui_chat_wrap (CJK/emoji = 2, combining marks = 0). Exposed so the
 * markdown table renderer pads cells to the columns the chat pane
 * actually counts.
 *
 * Return: display width in columns. Never fails.
 */
size_t tui_chat_display_width(const char *s, size_t len);

/**
 * tui_chat_truncate_width - truncate UTF-8 text to a display width
 * @text: NUL-terminated input; non-NULL.
 * @max_w: maximum display columns of the result; 0 yields "".
 * @out: caller-owned buffer receiving the NUL-terminated result.
 * @out_cap: capacity of @out in bytes; must be >= 1.
 *
 * Cuts at a codepoint boundary (never splits a UTF-8 sequence) and
 * appends "…" when anything was cut. Widths come from the same
 * locale-independent table as tui_chat_display_width. If the marker
 * does not fit in @out_cap it is omitted.
 *
 * Return: bytes written excluding the trailing NUL.
 */
size_t tui_chat_truncate_width(const char *text, size_t max_w,
                               char *out, size_t out_cap);

/**
 * tui_chat_total_lines - total wrapped line count across all blocks
 * @chat: scrollback; non-NULL.
 * @width: line width in columns; must be >= 1.
 *
 * Uses each block's render line count (marker rows included) and counts
 * a blank separator line after every block — exactly what the renderer
 * draws, so scroll clamping matches pixels.
 *
 * Return: total line count (>= 1 for an empty scrollback: one blank line).
 */
size_t tui_chat_total_lines(TuiChat *chat, size_t width);

/**
 * tui_chat_view_clamp - clamp a scroll offset to a viewport
 * @chat: scrollback; non-NULL.
 * @width: line width in columns; must be >= 1.
 * @visible: viewport height in lines; must be >= 1.
 * @top: requested top line (the row that should be at the viewport top).
 *
 * Return: clamped top line in [0, max(0, total - visible)]. The renderer
 *   calls this on every scroll input and resize.
 */
size_t tui_chat_view_clamp(TuiChat *chat, size_t width,
                           size_t visible, size_t top);

#endif /* ECHO_TUI_CHAT_H */
