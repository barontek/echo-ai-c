/*
 * metrics.c - Prometheus-text in-memory metrics registry (counters and
 * histograms) with silent-drop semantics on full registry or OOM.
 * Depends on: stdio (open_memstream), string_utils.h (str_dup).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics.h"
#include "string_utils.h"

#ifdef METRICS_TEST
static int metrics_alloc_counter = 0;
static int metrics_alloc_fail_at = -1;

void metrics_test_set_alloc_fail(int nth_allocation)
{
    metrics_alloc_counter = 0;
    metrics_alloc_fail_at = nth_allocation;
}

static void *metrics_test_calloc(size_t nmemb, size_t size)
{
    metrics_alloc_counter++;
    if (metrics_alloc_counter == metrics_alloc_fail_at) return NULL;
    return calloc(nmemb, size);
}

static char *metrics_test_strdup(const char *s)
{
    metrics_alloc_counter++;
    if (metrics_alloc_counter == metrics_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define calloc metrics_test_calloc
#define str_dup metrics_test_strdup
#endif

#define MAX_METRICS 64

typedef struct {
    char *name;
    char *help;
    char *type;
    long long count;
} CounterMetric;

typedef struct {
    char *name;
    char *help;
    double *buckets;
    int bucket_count;
    long long *bucket_counts;
    long long total_count;
    double total_sum;
} HistogramMetric;

struct Metrics {
    CounterMetric counters[MAX_METRICS];
    int counters_count;
    HistogramMetric histograms[MAX_METRICS];
    int histograms_count;
};

Metrics *metrics_create(void)
{
    return calloc(1, sizeof(Metrics));
}

void metrics_destroy(Metrics *m)
{
    if (!m) return;
    for (int i = 0; i < m->counters_count; i++)
    {
        free(m->counters[i].name);
        free(m->counters[i].help);
        free(m->counters[i].type);
    }
    for (int i = 0; i < m->histograms_count; i++)
    {
        free(m->histograms[i].name);
        free(m->histograms[i].help);
        free(m->histograms[i].buckets);
        free(m->histograms[i].bucket_counts);
    }
    free(m);
}

int metrics_counter_inc(Metrics *m, const char *name, const char *help)
{
    if (!m || !name) return -1;
    for (int i = 0; i < m->counters_count; i++)
    {
        if (strcmp(m->counters[i].name, name) == 0)
        {
            m->counters[i].count++;
            return 0;
        }
    }
    if (m->counters_count >= MAX_METRICS) return -1;

    char *n = str_dup(name);
    char *h = str_dup(help ? help : "");
    char *t = str_dup("counter");
    if (!n || !h || !t)
    {
        /* All-or-nothing: a partially allocated series must not be
         * committed to the registry (it would dangle on later frees). */
        free(n);
        free(h);
        free(t);
        return -1;
    }
    CounterMetric *cm = &m->counters[m->counters_count++];
    cm->name = n;
    cm->help = h;
    cm->type = t;
    cm->count = 1;
    return 0;
}

int metrics_histogram_observe(Metrics *m, const char *name, const char *help,
                              double value, const double *buckets, int bucket_count)
{
    if (!m || !name) return -1;
    int idx = -1;
    for (int i = 0; i < m->histograms_count; i++)
    {
        if (strcmp(m->histograms[i].name, name) == 0)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        if (m->histograms_count >= MAX_METRICS) return -1;

        char *n = str_dup(name);
        char *h = str_dup(help ? help : "");
        double *b = calloc((size_t)bucket_count, sizeof(double));
        long long *bc = calloc((size_t)bucket_count, sizeof(long long));
        if (!n || !h || !b || !bc)
        {
            /* All-or-nothing, same as the counter path: never commit a
             * histogram missing any of its four allocations. */
            free(n);
            free(h);
            free(b);
            free(bc);
            return -1;
        }
        HistogramMetric *hm = &m->histograms[m->histograms_count++];
        hm->name = n;
        hm->help = h;
        hm->bucket_count = bucket_count;
        hm->buckets = b;
        hm->bucket_counts = bc;
        for (int i = 0; i < bucket_count; i++)
            hm->buckets[i] = buckets[i];
        idx = m->histograms_count - 1;
    }

    HistogramMetric *hm = &m->histograms[idx];
    hm->total_count++;
    hm->total_sum += value;
    for (int i = 0; i < hm->bucket_count; i++)
    {
        if (value <= hm->buckets[i])
        {
            hm->bucket_counts[i]++;
            break;
        }
    }
    return 0;
}

char *metrics_render(Metrics *m)
{
    if (!m) return str_dup("");

    char *buf = NULL;
    size_t cap = 0;
    FILE *stream = open_memstream(&buf, &cap);
    if (!stream) return NULL;

    for (int i = 0; i < m->counters_count; i++)
    {
        CounterMetric *cm = &m->counters[i];
        if (cm->help && cm->help[0])
            fprintf(stream, "# HELP %s %s\n", cm->name, cm->help);
        fprintf(stream, "# TYPE %s counter\n", cm->name);
        fprintf(stream, "%s %lld\n", cm->name, cm->count);
    }

    for (int i = 0; i < m->histograms_count; i++)
    {
        HistogramMetric *hm = &m->histograms[i];
        if (hm->help && hm->help[0])
            fprintf(stream, "# HELP %s %s\n", hm->name, hm->help);
        fprintf(stream, "# TYPE %s histogram\n", hm->name);
        fprintf(stream, "%s_count %lld\n", hm->name, hm->total_count);
        fprintf(stream, "%s_sum %.0f\n", hm->name, hm->total_sum);
        for (int j = 0; j < hm->bucket_count; j++)
        {
            fprintf(stream, "%s_bucket{le=\"%.0f\"} %lld\n",
                    hm->name, hm->buckets[j], hm->bucket_counts[j]);
        }
        fprintf(stream, "%s_bucket{le=\"+Inf\"} %lld\n", hm->name, hm->total_count);
    }

    fclose(stream);
    return buf;
}