/*
 * test_tui_events.c - ring push/pop semantics, round-trip answering, and
 * allocation-failure injection on the push path (the multi-allocation
 * commit site of the TUI — same bug class as the metrics.c incidents).
 *
 * The fault-injection hooks (tui_events_test_calloc/strdup) count calls
 * and fail on a chosen one; TUI_EVENTS_TEST routes the module's
 * calloc/str_dup through them. Depends on: check, pthreads.
 */

#define _GNU_SOURCE
#include <check.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#include "tui/tui_events.h"

/* ---- fault-injection allocator shims ---- */
static int fail_at = -1;
static int call_count = 0;

void *tui_events_test_calloc(size_t nmemb, size_t size)
{
    call_count++;
    if (call_count == fail_at) return NULL;
    return calloc(nmemb, size);
}

char *tui_events_test_strdup(const char *s)
{
    call_count++;
    if (call_count == fail_at) return NULL;
    char *copy = malloc(strlen(s) + 1);
    if (!copy) return NULL;
    strcpy(copy, s);
    return copy;
}

static void reset_fault_injection(void)
{
    fail_at = -1;
    call_count = 0;
}

/* Fail the Nth allocation from now on (1 = the very next one). Setup
 * (tui_events_init) has already consumed allocations, so the tests must
 * never hardcode absolute call numbers. */
static void set_fail_at(int n)
{
    fail_at = call_count + n;
}

/* ---- fixtures ---- */
static TuiEvents *evs;

static void setup_ring(void)
{
    reset_fault_injection();
    evs = tui_events_init(4);
    ck_assert_ptr_nonnull(evs);
}

static void teardown_ring(void)
{
    tui_events_destroy(evs);
    evs = NULL;
}

/* ---- basic ring semantics ---- */

START_TEST(test_init_rejects_zero_capacity)
{
    ck_assert_ptr_null(tui_events_init(0));
}
END_TEST

START_TEST(test_push_pop_roundtrip_fifo)
{
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "one", NULL));
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "two", "extra"));
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_TITLE, "three", NULL));

    TuiEvent *e = tui_events_pop(evs);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(e->type, TUI_EV_CHUNK);
    ck_assert_str_eq(e->text, "one");
    ck_assert_ptr_null(e->extra);
    tui_event_free(e);

    e = tui_events_pop(evs);
    ck_assert_int_eq(e->type, TUI_EV_CHUNK);
    ck_assert_str_eq(e->text, "two");
    ck_assert_str_eq(e->extra, "extra");
    tui_event_free(e);

    e = tui_events_pop(evs);
    ck_assert_int_eq(e->type, TUI_EV_TITLE);
    ck_assert_str_eq(e->text, "three");
    tui_event_free(e);

    ck_assert_int_eq(tui_events_empty(evs), 1);
    ck_assert_ptr_null(tui_events_pop(evs));
}
END_TEST

START_TEST(test_pop_does_not_leak_payload_to_push_caller)
{
    /* The event owns its strings: pushing a caller-owned buffer and then
     * overwriting that buffer must not change what pop() returns. */
    char *buf = strdup("original");
    TuiEvent *e = tui_events_push(evs, TUI_EV_CHUNK, buf, NULL);
    ck_assert_ptr_nonnull(e);
    strcpy(buf, "mutated");
    free(buf);

    e = tui_events_pop(evs);
    ck_assert_str_eq(e->text, "original");
    tui_event_free(e);
}
END_TEST

typedef struct {
    TuiEvents *evs;
    TuiEvent *result;
    pthread_t thread;
} PushCtx;

static void *blocking_push_thread(void *arg)
{
    PushCtx *p = arg;
    p->result = tui_events_push(p->evs, TUI_EV_CHUNK, "blocked", NULL);
    return NULL;
}

START_TEST(test_push_blocks_when_full)
{
    for (int i = 0; i < 4; i++)
    {
        ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "x", NULL));
    }
    PushCtx p = {.evs = evs, .result = NULL};
    ck_assert_int_eq(pthread_create(&p.thread, NULL, blocking_push_thread, &p), 0);

    usleep(50000); /* let the pusher block on the full ring */

    TuiEvent *e = tui_events_pop(evs); /* free one slot */
    ck_assert_ptr_nonnull(e);
    tui_event_free(e);

    pthread_join(p.thread, NULL);
    ck_assert_ptr_nonnull(p.result); /* the blocked push completed */

    /* FIFO: the remaining queue is "x","x","x","blocked" */
    for (int i = 0; i < 3; i++)
    {
        e = tui_events_pop(evs);
        ck_assert_ptr_nonnull(e);
        ck_assert_str_eq(e->text, "x");
        tui_event_free(e);
    }
    e = tui_events_pop(evs);
    ck_assert_ptr_nonnull(e);
    ck_assert_str_eq(e->text, "blocked");
    tui_event_free(e);
    ck_assert_int_eq(tui_events_empty(evs), 1);
}
END_TEST

START_TEST(test_null_text_becomes_empty_string)
{
    TuiEvent *e = tui_events_push(evs, TUI_EV_CHUNK, NULL, NULL);
    ck_assert_ptr_nonnull(e);
    e = tui_events_pop(evs);
    ck_assert_str_eq(e->text, "");
    tui_event_free(e);
}
END_TEST

START_TEST(test_destroy_with_queued_events_is_clean)
{
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "a", NULL));
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_TOOL_START, "b", "{}"));
    /* teardown frees the queued events */
}
END_TEST

/* ---- round-trip events ---- */

typedef struct {
    TuiEvent *ev;
    char *answer_out;
    int result_out;
    int rc;
} WaitCtx;

static void *wait_thread(void *arg)
{
    WaitCtx *w = arg;
    w->rc = tui_event_wait_for_answer(w->ev, &w->answer_out, &w->result_out);
    return NULL;
}

START_TEST(test_roundtrip_answer_wakes_waiter)
{
    TuiEvent *ev = tui_events_push(evs, TUI_EV_ASK_USER, "question?", NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_int_eq(ev->round_trip, 1);

    /* The UI pops the event off the ring before answering it */
    ck_assert_ptr_eq(tui_events_pop(evs), ev);

    WaitCtx w = {.ev = ev, .answer_out = NULL, .result_out = -1, .rc = -1};
    pthread_t t;
    ck_assert_int_eq(pthread_create(&t, NULL, wait_thread, &w), 0);

    usleep(50000); /* let the waiter block */
    ck_assert_int_eq(tui_event_answer(ev, "the answer", 0), 0);
    pthread_join(t, NULL);

    ck_assert_int_eq(w.rc, 0);
    ck_assert_str_eq(w.answer_out, "the answer");
    free(w.answer_out);

    /* The worker owns the event after answering: free it here like the
     * worker would. */
    tui_event_free(ev);
}
END_TEST

START_TEST(test_roundtrip_answer_null_cancels)
{
    TuiEvent *ev = tui_events_push(evs, TUI_EV_APPROVAL, "bash", "{\"a\":1}");
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_eq(tui_events_pop(evs), ev);

    WaitCtx w = {.ev = ev, .answer_out = NULL, .result_out = -1, .rc = -1};
    pthread_t t;
    ck_assert_int_eq(pthread_create(&t, NULL, wait_thread, &w), 0);

    usleep(50000);
    ck_assert_int_eq(tui_event_answer(ev, NULL, 1), 0);
    pthread_join(t, NULL);

    ck_assert_int_eq(w.rc, 0);
    ck_assert_ptr_null(w.answer_out);
    ck_assert_int_eq(w.result_out, 1);
    tui_event_free(ev);
}
END_TEST

START_TEST(test_wait_on_non_roundtrip_returns_error)
{
    TuiEvent *ev = tui_events_push(evs, TUI_EV_CHUNK, "x", NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_eq(tui_events_pop(evs), ev);
    ck_assert_int_eq(tui_event_wait_for_answer(ev, NULL, NULL), -1);
    ck_assert_int_eq(tui_event_answer(ev, "y", 0), -1);
    tui_event_free(ev);
}
END_TEST

/* ---- allocation-failure injection ---- */

START_TEST(test_push_fails_cleanly_on_first_allocation)
{
    set_fail_at(1); /* calloc of the event struct fails */
    ck_assert_ptr_null(tui_events_push(evs, TUI_EV_CHUNK, "text", "extra"));
    ck_assert_int_eq(tui_events_empty(evs), 1);
}
END_TEST

START_TEST(test_push_fails_cleanly_on_text_copy_failure)
{
    set_fail_at(2); /* str_dup of text fails */
    ck_assert_ptr_null(tui_events_push(evs, TUI_EV_CHUNK, "text", "extra"));
    ck_assert_int_eq(tui_events_empty(evs), 1);
}
END_TEST

START_TEST(test_push_fails_cleanly_on_extra_copy_failure)
{
    set_fail_at(3); /* str_dup of extra fails; text copy must be freed */
    ck_assert_ptr_null(tui_events_push(evs, TUI_EV_CHUNK, "text", "extra"));
    ck_assert_int_eq(tui_events_empty(evs), 1);
}
END_TEST

START_TEST(test_push_fails_cleanly_on_ring_slot_failure)
{
    /* Capacity 4; fill it, then fail the very next allocation (the 5th
     * event's calloc) so the push aborts before blocking on the full
     * ring. */
    for (int i = 0; i < 4; i++)
    {
        ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "full", NULL));
    }
    set_fail_at(1);
    ck_assert_ptr_null(tui_events_push(evs, TUI_EV_CHUNK, "nope", NULL));
    /* The four earlier events are still there, order preserved */
    for (int i = 0; i < 4; i++)
    {
        TuiEvent *e = tui_events_pop(evs);
        ck_assert_ptr_nonnull(e);
        ck_assert_str_eq(e->text, "full");
        tui_event_free(e);
    }
    ck_assert_int_eq(tui_events_empty(evs), 1);
}
END_TEST

START_TEST(test_normal_operation_after_fault)
{
    set_fail_at(2);
    ck_assert_ptr_null(tui_events_push(evs, TUI_EV_CHUNK, "a", NULL));
    fail_at = -1;
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "b", NULL));
    TuiEvent *e = tui_events_pop(evs);
    ck_assert_str_eq(e->text, "b");
    tui_event_free(e);
}
END_TEST

/* ---- wake fd ---- */

START_TEST(test_wake_fd_becomes_readable_on_push)
{
    int fd = tui_events_wake_fd(evs);
    ck_assert_int_ge(fd, 0);
    ck_assert_ptr_nonnull(tui_events_push(evs, TUI_EV_CHUNK, "wake", NULL));

    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int rc = select(fd + 1, &set, NULL, NULL, &tv);
    ck_assert_int_eq(rc, 1);

    tui_events_drain_wake(evs);

    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    rc = select(fd + 1, &set, NULL, NULL, &tv);
    ck_assert_int_eq(rc, 0); /* drained: not readable anymore */
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tui_events");
    TCase *tc_ring = tcase_create("ring");
    tcase_add_checked_fixture(tc_ring, setup_ring, teardown_ring);
    tcase_add_test(tc_ring, test_init_rejects_zero_capacity);
    tcase_add_test(tc_ring, test_push_pop_roundtrip_fifo);
    tcase_add_test(tc_ring, test_pop_does_not_leak_payload_to_push_caller);
    tcase_add_test(tc_ring, test_null_text_becomes_empty_string);
    tcase_add_test(tc_ring, test_destroy_with_queued_events_is_clean);
    tcase_add_test(tc_ring, test_push_blocks_when_full);
    tcase_add_test(tc_ring, test_wake_fd_becomes_readable_on_push);
    tcase_set_timeout(tc_ring, 10);
    suite_add_tcase(s, tc_ring);

    TCase *tc_roundtrip = tcase_create("roundtrip");
    tcase_add_checked_fixture(tc_roundtrip, setup_ring, teardown_ring);
    tcase_add_test(tc_roundtrip, test_roundtrip_answer_wakes_waiter);
    tcase_add_test(tc_roundtrip, test_roundtrip_answer_null_cancels);
    tcase_add_test(tc_roundtrip, test_wait_on_non_roundtrip_returns_error);
    tcase_set_timeout(tc_roundtrip, 10);
    suite_add_tcase(s, tc_roundtrip);

    TCase *tc_fault = tcase_create("fault_injection");
    tcase_add_checked_fixture(tc_fault, setup_ring, teardown_ring);
    tcase_add_test(tc_fault, test_push_fails_cleanly_on_first_allocation);
    tcase_add_test(tc_fault, test_push_fails_cleanly_on_text_copy_failure);
    tcase_add_test(tc_fault, test_push_fails_cleanly_on_extra_copy_failure);
    tcase_add_test(tc_fault, test_push_fails_cleanly_on_ring_slot_failure);
    tcase_add_test(tc_fault, test_normal_operation_after_fault);
    suite_add_tcase(s, tc_fault);

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
