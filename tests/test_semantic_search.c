#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "tools/semantic_search.h"

extern void semantic_search_test_set_alloc_fail(int nth_allocation);
extern void semantic_search_test_reset(void);

START_TEST(test_sem_index_doc_alloc_fail_content_dup)
{
    semantic_search_test_set_alloc_fail(1);
    semantic_search_index_document("this should silently fail on strdup");
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_sem_index_doc_alloc_fail_add_term_first)
{
    semantic_search_index_document("first doc primes the index");
    semantic_search_test_set_alloc_fail(2);
    semantic_search_index_document("second doc triggers alloc fail in add_term");
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

START_TEST(test_sem_index_and_search)
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

Suite *semantic_search_suite(void)
{
    Suite *s = suite_create("SemanticSearch");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_sem_index_doc_alloc_fail_content_dup);
    tcase_add_test(tc, test_sem_index_doc_alloc_fail_add_term_first);
    tcase_add_test(tc, test_sem_term_freqs_oob);
    tcase_add_test(tc, test_sem_exec_uaf);
    tcase_add_test(tc, test_sem_index_and_search);
    suite_add_tcase(s, tc);
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
