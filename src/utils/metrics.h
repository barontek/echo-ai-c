#ifndef ECHO_METRICS_H
#define ECHO_METRICS_H

#include <stddef.h>

typedef struct Metrics Metrics;

Metrics *metrics_create(void);
void metrics_destroy(Metrics *m);

void metrics_counter_inc(Metrics *m, const char *name, const char *help);
void metrics_histogram_observe(Metrics *m, const char *name, const char *help,
                               double value, const double *buckets, int bucket_count);
char *metrics_render(Metrics *m);

#ifdef METRICS_TEST
void metrics_test_set_alloc_fail(int nth_allocation);
#endif

#endif