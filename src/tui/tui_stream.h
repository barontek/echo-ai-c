/*
 * tui_stream.h - stateful streamed-chunk classifier. The stream layers
 * emit explicit "<think>\n", thinking text, and "\n</think>\n\n" deltas;
 * without splitting, the closing delta would land in the assistant block
 * as three stray newlines (the "3 spaces between thinking and reply"
 * bug). The classifier is stateful because a marker may straddle two
 * deltas ("<thi" + "nk>"): a partial-marker tail is carried into the next
 * feed instead of being emitted, so markers resolve regardless of how the
 * provider slices the stream. Depends on: stdlib.
 */

#ifndef ECHO_TUI_STREAM_H
#define ECHO_TUI_STREAM_H

#include <stddef.h>

typedef enum {
    TUI_STREAM_PART_CONTENT, /* text for the assistant reply block */
    TUI_STREAM_PART_THINK    /* text inside a <think> block */
} TuiStreamPartKind;

/* A slice of classified text. start/len reference the classifier's
 * internal scratch buffer: valid until the next feed/flush/destroy call.
 * Callers must copy the bytes during the call. */
typedef struct {
    TuiStreamPartKind kind;
    const char *start;
    size_t len;
} TuiStreamPart;

typedef struct TuiStreamClassifier TuiStreamClassifier;

/**
 * tui_stream_classifier_create - allocate a fresh classifier
 *
 * Starts outside a think block. A single classifier is used for the
 * lifetime of one run; the carry state does not survive runs.
 *
 * Return: caller-owned classifier, or NULL on allocation failure.
 * Release with tui_stream_classifier_destroy(). Thread-safety: not
 * thread-safe — one producer (the run's chunk callback) and no sharing.
 */
TuiStreamClassifier *tui_stream_classifier_create(void);

/**
 * tui_stream_classifier_destroy - release a classifier
 * @cls: classifier to release, or NULL (no-op).
 *
 * Any carried tail is dropped.
 *
 * Return: void.
 */
void tui_stream_classifier_destroy(TuiStreamClassifier *cls);

/**
 * tui_stream_classifier_feed - classify one streamed chunk
 * @cls: classifier; non-NULL.
 * @chunk: delta bytes, borrowed; NULL or empty is a no-op.
 * @parts: caller-owned array receiving the classified slices.
 * @cap: capacity of @parts (at least 1; slices beyond @cap are dropped,
 *   not re-emitted).
 *
 * Merges the carried tail with @chunk before scanning, so a marker split
 * across two feeds resolves into one transition. Marker-adjacent
 * separator whitespace is trimmed: a delta consisting of only a marker
 * plus newlines produces no parts, only a state flip. A suffix of the
 * result that is a strict prefix of "<think>" or "</think>" is carried
 * into the next feed instead of being emitted.
 *
 * Return: number of parts written into @parts (0 when the chunk produced
 * no output, e.g. a bare marker delta).
 */
int tui_stream_classifier_feed(TuiStreamClassifier *cls, const char *chunk,
                               TuiStreamPart *parts, int cap);

/**
 * tui_stream_classifier_flush - emit the carried tail
 * @cls: classifier; non-NULL.
 * @part: out-param receiving the flushed slice, classified by the current
 *   think state. The slice references the classifier's carry buffer:
 *   copy it before destroy.
 *
 * Called once at end of stream so a partial marker that never resolved is
 * not lost.
 *
 * Return: 1 when a tail was flushed, 0 when there was nothing pending.
 */
int tui_stream_classifier_flush(TuiStreamClassifier *cls, TuiStreamPart *part);

#endif /* ECHO_TUI_STREAM_H */
