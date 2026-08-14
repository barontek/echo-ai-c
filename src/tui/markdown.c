/*
 * markdown.c - markdown classification for the chat pane: line kinds,
 * inline run tokenization (bold/italic/code/strike/links), and table
 * geometry. Deliberately a subset of CommonMark — enough to make LLM
 * output readable without a full block parser. Delimiter spans never
 * cross line boundaries; a delimiter only opens when a valid closer
 * exists later in the line, so a half-streamed "**bold" renders as
 * literal text until the closer arrives. Depends on: string.h, ctype.h,
 * tui_chat (display width).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "markdown.h"
#include "tui_chat.h"

#define MD_MAX_DEPTH 8   /* open inline delimiters per line */
#define MD_MAX_COLS  16  /* table columns; more splits as plain text */

typedef struct {
    char ch;        /* '*', '_' or '~' */
    unsigned bits;  /* the style the delimiter opened */
} MdDelim;

static int md_isalnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/* ASCII punctuation is backslash-escaped in CommonMark; the byte set
 * must stay literal in the source string (backslash doubled). */
static int md_ispunct(unsigned char c)
{
    static const char *p = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    return c >= 0x21 && c <= 0x7e && strchr(p, (char)c) != NULL;
}

static unsigned run_bits(size_t n)
{
    if (n >= 3) return MD_STYLE_BOLD | MD_STYLE_ITALIC;
    if (n == 2) return MD_STYLE_BOLD;
    return MD_STYLE_ITALIC;
}

/* Flanking check (closer=1 tests a closing delimiter at j, opener
 * otherwise). '*' and '~' may open mid-word; '_' may not (so
 * snake_case and URLs survive) — and '_' only closes before
 * non-alphanumerics. */
static int flank_ok(const char *s, size_t len, size_t j, size_t n,
                    char ch, int closer)
{
    if (closer)
    {
        if (j == 0 || s[j - 1] == ' ') return 0;
        if (ch == '_' && j + n < len && md_isalnum((unsigned char)s[j + n]))
            return 0;
        return 1;
    }
    if (j + n >= len || s[j + n] == ' ') return 0;
    if (ch == '_' && j > 0 && md_isalnum((unsigned char)s[j - 1]))
        return 0;
    return 1;
}

/* Does a valid closer for (ch, bits) exist at or after `from`? Code
 * spans are skipped, so "**a `b**`" does not open bold. */
static int find_closer(const char *s, size_t len, size_t from,
                       char ch, unsigned bits)
{
    size_t j = from;
    while (j < len)
    {
        if (s[j] == '`')
        {
            size_t m = 1;
            while (j + m < len && s[j + m] == '`') m++;
            if (m > 2) m = 2;
            /* the span closes at the next backtick run of the same
             * length; a different-length run is literal content */
            size_t q = j + m;
            int closed = 0;
            while (q < len)
            {
                if (s[q] == '`')
                {
                    size_t mm = 1;
                    while (q + mm < len && s[q + mm] == '`') mm++;
                    if (mm > 2) mm = 2;
                    if (mm == m)
                    {
                        j = q + mm;
                        closed = 1;
                        break;
                    }
                    q += mm;
                    continue;
                }
                q++;
            }
            if (!closed)
                j = len; /* unclosed span: nothing after it can match */
            continue;
        }
        if (s[j] == ch)
        {
            size_t n = 1;
            while (j + n < len && s[j + n] == ch) n++;
            if (n > 3) n = 3;
            unsigned bits2 = run_bits(n);
            if (ch == '~') bits2 = MD_STYLE_STRIKE;
            if (bits2 == bits && flank_ok(s, len, j, n, ch, 1))
                return 1;
            j += n;
            continue;
        }
        j++;
    }
    return 0;
}

static void md_emit(MdRun *runs, size_t *count, size_t cap,
                    size_t start, size_t len, unsigned style)
{
    if (len == 0 || *count >= cap) return;
    runs[*count].start = start;
    runs[*count].len = len;
    runs[*count].style = style;
    (*count)++;
}

size_t md_parse_inline(const char *s, size_t len, MdRun *runs, size_t cap)
{
    if (!s) return 0;
    if (!runs) cap = 0;

    size_t count = 0;
    size_t start = 0;   /* byte offset of the run currently being built */
    unsigned cur = 0;   /* style of that run */
    MdDelim stack[MD_MAX_DEPTH];
    size_t depth = 0;
    int in_code = 0;
    size_t code_len = 0;

    size_t i = 0;
    while (i < len)
    {
        char c = s[i];

        if (c == '\\' && i + 1 < len && md_ispunct((unsigned char)s[i + 1]))
        {
            /* Backslash-escaped punctuation renders as itself: the
             * escape joins the current run so styled text keeps its
             * style across the escaped char. */
            md_emit(runs, &count, cap, start, i - start, cur);
            md_emit(runs, &count, cap, i + 1, 1, cur);
            i += 2;
            start = i;
            continue;
        }

        if (c == '`')
        {
            size_t n = 1;
            while (i + n < len && s[i + n] == '`') n++;
            if (n > 2) n = 2; /* 3+ is a fence; the caller never parses fences */
            if (in_code)
            {
                if (n == code_len)
                {
                    md_emit(runs, &count, cap, start, i - start, cur);
                    i += n;
                    start = i;
                    in_code = 0;
                    cur &= ~(unsigned)MD_STYLE_CODE;
                }
                else
                {
                    i += n; /* mismatched run is literal code content */
                }
                continue;
            }
            md_emit(runs, &count, cap, start, i - start, cur);
            i += n;
            start = i;
            in_code = 1;
            code_len = n;
            cur |= MD_STYLE_CODE;
            continue;
        }

        if (in_code)
        {
            i++; /* code content is fully literal */
            continue;
        }

        if (c == '[')
        {
            size_t j = i + 1;
            while (j < len && s[j] != ']' && s[j] != '\n') j++;
            if (j < len && j + 1 < len && s[j + 1] == '(')
            {
                size_t k = j + 2;
                while (k < len && s[k] != ')' && s[k] != '\n') k++;
                if (k < len && s[k] == ')')
                {
                    md_emit(runs, &count, cap, start, i - start, cur);
                    md_emit(runs, &count, cap, i + 1, j - i - 1,
                            cur | MD_STYLE_LINK);
                    i = k + 1;
                    start = i;
                    continue;
                }
            }
            i++; /* malformed link renders literally */
            continue;
        }

        if (c == '*' || c == '_' || c == '~')
        {
            size_t n = 1;
            while (i + n < len && s[i + n] == c) n++;
            if (n > 3) n = 3;
            unsigned bits = c == '~' ? MD_STYLE_STRIKE : run_bits(n);

            if (flank_ok(s, len, i, n, c, 1))
            {
                /* Closer: pop the nearest exact entry; delimiters above
                 * it close along with it (their text was never styled
                 * as them). */
                ssize_t match = -1;
                for (size_t d = depth; d > 0; d--)
                {
                    if (stack[d - 1].ch == c && stack[d - 1].bits == bits)
                    {
                        match = (ssize_t)(d - 1);
                        break;
                    }
                }
                if (match >= 0)
                {
                    md_emit(runs, &count, cap, start, i - start, cur);
                    depth = (size_t)match;
                    cur = 0;
                    for (size_t d = 0; d < depth; d++)
                        cur |= stack[d].bits;
                    i += n;
                    start = i;
                    continue;
                }
            }
            if (flank_ok(s, len, i, n, c, 0) && depth < MD_MAX_DEPTH &&
                find_closer(s, len, i + n, c, bits))
            {
                md_emit(runs, &count, cap, start, i - start, cur);
                stack[depth].ch = c;
                stack[depth].bits = bits;
                depth++;
                cur |= bits;
                i += n;
                start = i;
                continue;
            }
            i += n; /* invalid delimiter stays literal in the current run */
            continue;
        }

        i++;
    }

    md_emit(runs, &count, cap, start, len - start, cur);
    return count;
}

/* ---- line classification ---- */

typedef struct {
    const char *p;
    size_t len;
} CellSpan;

/* Split a table row into trimmed cells. A leading '|' and a trailing
 * '|' (which yields an empty last cell) are dropped. '|' preceded by
 * '\' is content, not a separator; a newline ends the row. */
static size_t split_cells(const char *row, size_t len, CellSpan *cells,
                          size_t cap)
{
    size_t count = 0;
    size_t i = 0;
    if (i < len && row[i] == '|') i++;
    while (i < len && count < cap)
    {
        size_t b = i;
        while (i < len && row[i] != '\n' &&
               !(row[i] == '|' && (i == 0 || row[i - 1] != '\\')))
            i++;
        size_t e = i;
        while (b < e && row[b] == ' ') b++;
        while (e > b && row[e - 1] == ' ') e--;
        cells[count].p = row + b;
        cells[count].len = e - b;
        count++;
        if (i < len && row[i] == '|') i++;
        else break; /* newline or end of input: no more cells */
    }
    if (count > 0 && cells[count - 1].len == 0) count--;
    return count;
}

/* Separator cell: '---', '--:',':--' etc. — dashes with optional
 * colons, at least one dash. */
static int sep_cell(const CellSpan *c)
{
    size_t i = 0;
    if (i < c->len && c->p[i] == ':') i++;
    size_t dashes = 0;
    while (i < c->len && c->p[i] == '-') { i++; dashes++; }
    if (dashes == 0) return 0;
    if (i < c->len && c->p[i] == ':') i++;
    return i == c->len;
}

MdLineKind md_line_kind(const char *s, size_t len)
{
    if (!s) return MD_LINE_BLANK;
    size_t i = 0;
    while (i < len && i < 3 && s[i] == ' ') i++; /* CommonMark indent */
    if (i >= len) return MD_LINE_BLANK;
    char c = s[i];

    if (c == '#')
    {
        size_t j = i;
        while (j < len && s[j] == '#') j++;
        if (j - i <= 6 && (j == len || s[j] == ' '))
            return MD_LINE_HEADING;
        return MD_LINE_PLAIN;
    }
    if (c == '>') return MD_LINE_QUOTE;
    if (c == '-' || c == '*' || c == '+' || c == '_')
    {
        if (c != '+')
        {
            size_t j = i;
            while (j < len && s[j] == c) j++;
            if (j - i >= 3)
            {
                size_t k = j;
                while (k < len && s[k] == ' ') k++;
                if (k == len) return MD_LINE_HR;
            }
        }
        if (c != '_' && i + 1 < len && s[i + 1] == ' ')
            return MD_LINE_LIST;
        return MD_LINE_PLAIN;
    }
    if (c >= '0' && c <= '9')
    {
        size_t j = i;
        while (j < len && s[j] >= '0' && s[j] <= '9') j++;
        if (j - i <= 9 && j < len && (s[j] == '.' || s[j] == ')') &&
            (j + 1 == len || s[j + 1] == ' '))
            return MD_LINE_LIST;
        return MD_LINE_PLAIN;
    }
    if (c == '`' || c == '~')
    {
        size_t j = i;
        while (j < len && s[j] == c) j++;
        if (j - i >= 3) return MD_LINE_CODE_FENCE;
        return MD_LINE_PLAIN;
    }
    if (c == '|')
    {
        CellSpan cells[MD_MAX_COLS];
        size_t nc = split_cells(s + i, len - i, cells, MD_MAX_COLS);
        if (nc > 0)
        {
            int all_sep = 1;
            for (size_t k = 0; k < nc; k++)
            {
                if (!sep_cell(&cells[k]))
                {
                    all_sep = 0;
                    break;
                }
            }
            if (all_sep) return MD_LINE_TABLE_SEP;
            if (nc >= 1) return MD_LINE_TABLE_ROW;
        }
        return MD_LINE_PLAIN;
    }
    return MD_LINE_PLAIN;
}

/* ---- tables ---- */

MdTable *md_table_scan(const char *text, const size_t *starts, size_t nlines,
                       size_t first, size_t *rows)
{
    if (!text || !starts || !rows) return NULL;
    if (first + 2 > nlines) return NULL; /* need row + separator + sentinel */
    if (md_line_kind(text + starts[first], starts[first + 1] - starts[first]) !=
        MD_LINE_TABLE_ROW)
        return NULL;
    if (md_line_kind(text + starts[first + 1],
                     starts[first + 2] - starts[first + 1]) != MD_LINE_TABLE_SEP)
        return NULL;

    size_t count = 1;
    for (size_t k = first + 2; k < nlines; k++)
    {
        if (md_line_kind(text + starts[k], starts[k + 1] - starts[k]) !=
            MD_LINE_TABLE_ROW)
            break;
        count++;
    }

    CellSpan sep[MD_MAX_COLS];
    size_t ncols = split_cells(text + starts[first + 1],
                               starts[first + 2] - starts[first + 1],
                               sep, MD_MAX_COLS);
    if (ncols == 0) return NULL;

    /* One allocation for struct + arrays keeps the commit atomic. */
    MdTable *t = calloc(1, sizeof(MdTable) + ncols * sizeof(size_t) +
                          ncols * sizeof(unsigned));
    if (!t) return NULL;
    t->ncols = ncols;
    t->nrows = count;
    t->widths = (size_t *)(t + 1);
    t->align = (unsigned *)(t->widths + ncols);

    for (size_t c = 0; c < ncols; c++)
    {
        const CellSpan *sc = &sep[c];
        int left = sc->len > 0 && sc->p[0] == ':';
        int right = sc->len > 0 && sc->p[sc->len - 1] == ':';
        if (left && right) t->align[c] = 1;
        else if (right) t->align[c] = 2;
    }

    for (size_t r = 0; r < count; r++)
    {
        /* Rows are not contiguous: the separator line at first + 1
         * sits between the first row and the rest. */
        size_t rl = (r == 0) ? first : first + r + 1;
        CellSpan cells[MD_MAX_COLS];
        size_t rc = split_cells(text + starts[rl],
                                starts[rl + 1] - starts[rl],
                                cells, MD_MAX_COLS);
        for (size_t c = 0; c < ncols && c < rc; c++)
        {
            size_t w = tui_chat_display_width(cells[c].p, cells[c].len);
            if (w > t->widths[c]) t->widths[c] = w;
        }
    }

    *rows = count;
    return t;
}

void md_table_free(MdTable *t)
{
    free(t); /* single allocation: struct + arrays */
}

static size_t md_ch(char *out, size_t cap, size_t o, char ch)
{
    if (o + 1 < cap) out[o++] = ch;
    return o;
}

static size_t md_lit(char *out, size_t cap, size_t o, const char *lit)
{
    for (size_t i = 0; lit[i]; i++)
        o = md_ch(out, cap, o, lit[i]);
    return o;
}

/* Write a cell, unescaping \| (the width math counts the raw text, so
 * escaped pipes pad one column wider than strictly needed — invisible). */
static size_t md_cell(char *out, size_t cap, size_t o,
                      const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '|') continue;
        o = md_ch(out, cap, o, s[i]);
    }
    return o;
}

size_t md_table_render_row(char *out, size_t cap, const char *row,
                           size_t len, const MdTable *tbl)
{
    if (cap == 0) return 0;
    if (!out) return 0;
    out[0] = '\0';
    if (!tbl || !row) return 0;

    CellSpan cells[MD_MAX_COLS];
    size_t rc = split_cells(row, len, cells, MD_MAX_COLS);

    size_t o = 0;
    for (size_t c = 0; c < tbl->ncols; c++)
    {
        o = md_lit(out, cap, o, c == 0 ? "\xE2\x94\x82 " : " \xE2\x94\x82 ");
        size_t cw = c < rc ? tui_chat_display_width(cells[c].p, cells[c].len) : 0;
        size_t pad = tbl->widths[c] > cw ? tbl->widths[c] - cw : 0;
        size_t left = 0;
        if (tbl->align[c] == 2) left = pad;
        else if (tbl->align[c] == 1) left = pad / 2;
        for (size_t k = 0; k < left; k++)
            o = md_ch(out, cap, o, ' ');
        if (c < rc)
            o = md_cell(out, cap, o, cells[c].p, cells[c].len);
        size_t right = c + 1 == tbl->ncols ? 0 : pad - left;
        for (size_t k = 0; k < right; k++)
            o = md_ch(out, cap, o, ' ');
    }
    o = md_lit(out, cap, o, " \xE2\x94\x82");
    if (o >= cap) o = cap - 1;
    out[o] = '\0';
    return o;
}
