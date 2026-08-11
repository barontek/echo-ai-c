#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "tools/semantic_search.h"

/* test_semantic_search - semantic_search tool unit tests. Depends on: check, the module under test. */
extern void semantic_search_test_set_alloc_fail(int nth_allocation);
extern void semantic_search_test_set_realloc_fail(int nth_realloc);
extern void semantic_search_test_reset(void);

START_TEST(test_sem_index_doc_alloc_fail_content_dup)
{
    /* E8: the content str_dup failure must return -1 and commit nothing —
     * the old test asserted nothing at all. The failed document's words
     * must be absent from every later search. */
    semantic_search_test_set_alloc_fail(1);
    int rc = semantic_search_index_document(
        "this should silently fail on strdup");
    semantic_search_test_set_alloc_fail(-1);
    ck_assert_int_eq(rc, -1);

    semantic_search_index_document("machine learning");
    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);
    ToolResult *r = t->execute(t, "{\"query\":\"silently\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_null(strstr(r->content, "silently"));
    tool_result_free(r);
    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_sem_index_doc_alloc_fail_add_term_first)
{
    /* E8: a failed add_term used to skip the term silently and still
     * commit the document (half-indexed). It must return -1 and leave the
     * document absent from later searches. */
    semantic_search_index_document("first doc primes the index");
    semantic_search_test_set_alloc_fail(2);
    int rc = semantic_search_index_document(
        "second doc triggers alloc fail in add_term");
    semantic_search_test_set_alloc_fail(-1);
    ck_assert_int_eq(rc, -1);

    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);
    ToolResult *r = t->execute(t, "{\"query\":\"triggers\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_null(strstr(r->content, "triggers"));
    tool_result_free(r);
    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_sem_add_term_realloc_fail_keeps_rows_consistent)
{
    /* Old code: add_term bumped term_count before resizing every row, so
     * a failed realloc left the row one int too small while term_count
     * had already grown; compute_tfidf then read past the row end (ASan
     * heap-buffer-overflow on the search below). New code: the term is
     * only committed after every row has room for it. */
    semantic_search_index_document("alpha beta gamma delta");
    semantic_search_index_document("alpha beta gamma delta epsilon");
    semantic_search_test_set_realloc_fail(1);
    semantic_search_index_document("alpha beta gamma delta epsilon zeta");

    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);
    ToolResult *r = t->execute(t, "{\"query\":\"zeta\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);
    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_sem_term_freqs_oob)
{
    /* Old code: calloc(term_count ?: 1, sizeof(int)) under-allocates when
     * multiple new terms are added during a single document's loop.
     * term_freqs[0][t] for t > 0 is an OOB write.  New code allocates
     * MAX_TOKENS.  Under ASan the old code crashes here. */
    semantic_search_index_document("tokenization produces many distinct terms");

    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);
    ToolResult *r = t->execute(t, "{\"query\":\"tokenization\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);
    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_sem_exec_uaf)
{
    /* Old code: cJSON_Delete(args) frees the parsed JSON object, but
     * the 'query' pointer aliases into it.  The subsequent tokenize()
     * reads freed memory (heap-use-after-free under ASan).  Fix:
     * str_dup the query string before deleting the args tree. */
    semantic_search_index_document("machine learning uses backpropagation");

    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"query\":\"backpropagation\",\"top_k\":1}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    tool_result_free(r);

    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_sem_index_and_search_returns_results_for_known_terms)
{
    semantic_search_index_document("machine learning is transforming technology");
    semantic_search_index_document("deep neural networks use gradient descent");
    semantic_search_index_document("reinforcement learning trains agents");

    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"query\":\"learning\",\"top_k\":2}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    tool_result_free(r);

    r = t->execute(t, "{\"query\":\"xyzzy\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

/* B2: the index teardown must free every allocation and leave the index
 * empty and reusable for a fresh pass. On the old code the teardown only
 * existed as a test hook; now it is the public semantic_search_free_index
 * (also called by the tool destroy), so a second indexing pass after the
 * teardown must behave exactly like a fresh index. */
START_TEST(test_sem_free_index_releases_and_reuses)
{
    semantic_search_index_document("machine learning is transforming technology");
    semantic_search_index_document("reinforcement learning trains agents");
    semantic_search_free_index();

    /* fresh pass: one document in, one result out — no residue from the
     * freed index */
    ck_assert_int_eq(semantic_search_index_document("deep neural networks"),
                     0);
    Tool *t = tool_semantic_search_create(NULL);
    ck_assert_ptr_nonnull(t);
    ToolResult *r = t->execute(t, "{\"query\":\"neural\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "neural") != NULL);
    tool_result_free(r);

    r = t->execute(t, "{\"query\":\"learning\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_null(strstr(r->content, "learning"));
    tool_result_free(r);

    t->destroy(t);
    semantic_search_test_reset();
}
END_TEST

Suite *semantic_search_suite(void)
{
    Suite *s = suite_create("SemanticSearch");

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_add_test(tc_fault, test_sem_index_doc_alloc_fail_content_dup);
    tcase_add_test(tc_fault, test_sem_index_doc_alloc_fail_add_term_first);
    tcase_add_test(tc_fault, test_sem_add_term_realloc_fail_keeps_rows_consistent);
    suite_add_tcase(s, tc_fault);

    TCase *tc_integration = tcase_create("Integration");
    tcase_add_test(tc_integration, test_sem_term_freqs_oob);
    tcase_add_test(tc_integration, test_sem_exec_uaf);
    tcase_add_test(tc_integration, test_sem_index_and_search_returns_results_for_known_terms);
    tcase_add_test(tc_integration, test_sem_free_index_releases_and_reuses);
    suite_add_tcase(s, tc_integration);

    return s;
}

int main(void)
{
    Suite *s = semantic_search_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
