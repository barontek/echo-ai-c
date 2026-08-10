/*
 * metrics.h - Prometheus-text in-memory metrics registry: counters and
 * histograms with bounded capacity. Depends on: stddef.h, string_utils.h.
 */

#ifndef ECHO_METRICS_H
#define ECHO_METRICS_H

#include <stddef.h>

typedef struct Metrics Metrics;

/**
 * metrics_create - allocate an empty metrics registry
 *
 * Return: caller-owned Metrics with no registered series, or NULL on
 * allocation failure. Release with metrics_destroy(). No shared state;
 * the returned registry is standalone.
 */
Metrics *metrics_create(void);

/**
 * metrics_destroy - free a metrics registry and all its series
 * @m: registry to free; NULL is a safe no-op.
 *
 * Frees every counter/histogram name, help string, and copied bucket
 * array owned by the registry.
 *
 * Return: void; never fails.
 */
void metrics_destroy(Metrics *m);

/**
 * metrics_counter_inc - record one increment on a counter
 * @m: registry to update; NULL is a no-op.
 * @name: series name (the identity key); NULL is a no-op.
 * @help: optional help text stored with the series; NULL means no help
 *   line. Only used when the series is first created.
 *
 * A series already named `name` is incremented; otherwise a new counter
 * starts at 1. A partially built series is fully freed on failure.
 *
 * Return: 0 when the increment was recorded, -1 when it was dropped
 * (NULL arguments, registry full at 64 series, or an allocation
 * failure). Thread-safety: mutates the registry without locking; the
 * caller must serialize access.
 */
int metrics_counter_inc(Metrics *m, const char *name, const char *help);

/**
 * metrics_histogram_observe - record one observation on a histogram
 * @m: registry to update; NULL is a no-op.
 * @name: series name (the identity key); NULL is a no-op.
 * @help: optional help text stored with the series; NULL means no help
 *   line. Only used when the series is first created.
 * @value: observed value.
 * @buckets: ascending bucket upper bounds; copied by the registry on
 *   first sighting, so the caller retains ownership of the array.
 * @bucket_count: number of bucket bounds.
 *
 * A series already named `name` records the observation; otherwise a
 * new histogram is created. value is counted in the first bucket with
 * value <= bound (and in the implicit +Inf bucket via the total count).
 * A partially built series is fully freed on failure.
 *
 * Return: 0 when the observation was recorded, -1 when it was dropped
 * (NULL arguments, registry full at 64 series, or an allocation
 * failure). Thread-safety: mutates the registry without locking; the
 * caller must serialize access.
 */
int metrics_histogram_observe(Metrics *m, const char *name, const char *help,
                              double value, const double *buckets, int bucket_count);

/**
 * metrics_render_new - serialize the registry as Prometheus text format
 * @m: registry to render; NULL yields the empty string.
 *
 * Emits # HELP (when present) / # TYPE lines followed by one line per
 * counter and per histogram (_count/_sum/_bucket{le=...}).
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 * (free with free()), or NULL on allocation failure. Thread-safety: the
 * registry must not be concurrently mutated while rendering.
 */
char *metrics_render_new(Metrics *m);

#ifdef METRICS_TEST
/**
 * metrics_test_set_alloc_fail - arm the allocation-failure hook
 * @nth_allocation: 1-based index of the next metrics_* internal
 *   allocation that should fail; -1 disables fault injection.
 *
 * Test-only hook for allocation-failure regression tests (AGENTS.md
 * section 11). The call counter resets on every arm, so the index
 * counts from the next metrics call onward.
 *
 * Return: void; never fails.
 */
void metrics_test_set_alloc_fail(int nth_allocation);
#endif

#endif