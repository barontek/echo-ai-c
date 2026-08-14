/*
 * markdown.h - markdown rendering contract for the chat pane: line
 * classification, inline run tokenization, and table geometry. Pure logic
 * (no notcurses types) so it is fully unit-testable; the renderer in
 * tui.c maps MdStyle bits to colors/styles. Depends on: stddef, stdint,
 * tui_chat (display-width helper).
 */

#ifndef ECHO_TUI_MARKDOWN_H
#define ECHO_TUI_MARKDOWN_H

#include <stddef.h>
#include <stdint.h>

/* Style bits a single run of text can carry. The renderer decides the
 * concrete color/style; the bits only say *what* the text is. */
typedef enum {
    MD_STYLE_BOLD = 1 << 0,
    MD_STYLE_ITALIC = 1 << 1,
    MD_STYLE_CODE = 1 << 2,   /* inline code span or fenced-block content */
    MD_STYLE_STRIKE = 1 << 3,
    MD_STYLE_LINK = 1 << 4,   /* link text (renderer underlines) */
    MD_STYLE_HEADING = 1 << 5, /* whole heading line (accent + bold) */
    MD_STYLE_DIM = 1 << 6     /* quote / fence lines (muted) */
} MdStyle;

typedef struct {
    size_t start;   /* byte offset into the parsed line */
    size_t len;     /* byte length */
    unsigned style; /* MdStyle bits */
} MdRun;

/* Line-level classification (one line of a chat block). */
typedef enum {
    MD_LINE_PLAIN = 0,
    MD_LINE_BLANK,      /* whitespace only */
    MD_LINE_HEADING,    /* 1-6 '#' + space (or bare '#') */
    MD_LINE_QUOTE,      /* '>' [space] */
    MD_LINE_LIST,       /* '- ' '* ' '+ ' or 'N.' / 'N)' */
    MD_LINE_HR,         /* '---' / '***' / '___' */
    MD_LINE_CODE_FENCE, /* '```' or '~~~' opener/closer */
    MD_LINE_TABLE_ROW,  /* '| a | b |' */
    MD_LINE_TABLE_SEP   /* '| --- | :--: |' alignment row */
} MdLineKind;

/**
 * md_parse_inline - tokenize one line into styled runs
 * @line: NUL-terminated line text; non-NULL.
 * @len: byte length of @line.
 * @runs: caller-owned output array; receives up to @cap runs.
 * @cap: capacity of @runs.
 *
 * Recognized delimiters: double star/underscore bold, single star or
 * underscore italic, triple bold+italic, ` and `` code spans (content
 * is literal), ~~ strike, [t](u) links (rendered as the text), and
 * backslash escapes before ASCII punctuation. `*` opens only before
 * non-space and closes only after non-space; `_` additionally refuses
 * to open/close inside an alphanumeric run (so snake_case and URLs
 * survive). Unclosed delimiters render literally instead of leaking
 * styling. The output covers [0, len) contiguously; a run of length 0
 * never appears.
 *
 * Return: run count (<= @cap; excess runs are dropped and the text is
 *   covered by the emitted prefix). Never fails.
 */
size_t md_parse_inline(const char *line, size_t len, MdRun *runs, size_t cap);

/**
 * md_line_kind - classify one line
 * @line: NUL-terminated line text; non-NULL.
 * @len: byte length of @line.
 *
 * A line is classified by its leading construct only; a table row is
 * only a row when a separator follows (the renderer checks that via
 * md_table_scan). Up to three leading spaces are ignored (CommonMark
 * indentation).
 *
 * Return: the line kind. Never fails.
 */
MdLineKind md_line_kind(const char *line, size_t len);

typedef struct {
    size_t ncols;  /* column count (from the separator row) */
    size_t nrows;  /* row count (separator excluded) */
    size_t *widths; /* display columns per column; in the struct block */
    unsigned *align; /* per column: 0 left, 1 center, 2 right */
} MdTable;

/**
 * md_table_scan - detect a table starting at line @first
 * @text: the whole block text; non-NULL.
 * @starts: line-start offsets into @text (tui_chat_block_line_starts
 *   layout: @nlines + 1 entries, last = strlen(text)); non-NULL.
 * @nlines: number of content lines in @text.
 * @first: index of the candidate first row line.
 * @rows: out-param receiving the number of row lines (separator line
 *   excluded); untouched on failure.
 *
 * Line @first must be a table row and line @first + 1 a table
 * separator; following row lines extend the table. Column count and
 * alignment come from the separator, widths from all rows. Caller must
 * skip @rows + 1 lines (the separator line renders nothing).
 *
 * Return: caller-owned MdTable (single allocation; release with
 *   md_table_free), or NULL when no table starts at @first (render the
 *   line plainly).
 */
MdTable *md_table_scan(const char *text, const size_t *starts, size_t nlines,
                       size_t first, size_t *rows);

/**
 * md_table_render_row - render one table row into a buffer
 * @out: caller-owned output buffer, NUL-terminated on return.
 * @cap: capacity of @out (bytes).
 * @row: NUL-terminated raw row text; non-NULL.
 * @len: byte length of @row.
 * @tbl: geometry from md_table_scan; non-NULL.
 *
 * Splits @row into cells (unescaping \|), trims them, pads each to its
 * column width per alignment, and joins with '|' (box-drawing). Missing
 * trailing cells render empty. Longer output than @cap is truncated
 * (NUL-terminated).
 *
 * Return: bytes written (excluding the NUL), capped at @cap - 1.
 */
size_t md_table_render_row(char *out, size_t cap, const char *row, size_t len,
                           const MdTable *tbl);

/**
 * md_table_free - release a table from md_table_scan
 * @tbl: table to release, or NULL (no-op).
 *
 * Return: void.
 */
void md_table_free(MdTable *tbl);

#endif /* ECHO_TUI_MARKDOWN_H */
