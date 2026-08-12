/*
 * tui_stream.h - streamed-chunk classifier: splits LLM stream deltas into
 * think-block and content parts, stripping the provider's <think>/
 * </think> markers and their surrounding separator whitespace. Pure logic
 * (no allocation, no I/O) so it is fully unit-testable.
 * Depends on: stddef.h.
 */

#ifndef ECHO_TUI_STREAM_H
#define ECHO_TUI_STREAM_H

#include <stddef.h>

typedef enum {
    TUI_STREAM_PART_THINK,   /* content inside a <think> block */
    TUI_STREAM_PART_CONTENT  /* regular assistant content */
} TuiStreamPartKind;

/* A slice of the input chunk (borrowed — points into the original). */
typedef struct {
    TuiStreamPartKind kind;
    const char *start;
    size_t len;
} TuiStreamPart;

/**
 * tui_stream_split - classify and split one streamed chunk
 * @chunk: NUL-terminated delta from the provider; non-NULL.
 * @in_think: whether the stream is currently inside a <think> block.
 * @parts: caller-owned array receiving up to 2 parts.
 * @out_think: out-param receiving the new in-think state.
 *
 * "<think>"/"</think>" markers delimit think blocks. A chunk containing an
 * opening marker is split before the marker (content) and after it
 * (think); a closing marker splits think-before (trimmed of trailing
 * whitespace) and content-after (trimmed of leading whitespace). The
 * marker text itself is never included, so stored blocks stay clean.
 * Provider emissions like "<think>\n" or "\n</think>\n\n" therefore
 * produce zero parts and merely flip the state. Chunks containing both
 * markers are handled by the first marker encountered; the remainder is
 * passed through untrimmed (providers never emit both in one chunk).
 *
 * Return: number of parts written (0-2). @out_think is always written.
 */
int tui_stream_split(const char *chunk, int in_think,
                     TuiStreamPart parts[2], int *out_think);

#endif /* ECHO_TUI_STREAM_H */
