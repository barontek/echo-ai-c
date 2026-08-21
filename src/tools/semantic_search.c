/*
 * semantic_search.c - Process-wide TF-IDF document index and the
 * semantic_search tool that queries it. Depends on: tool.h (cJSON),
 * string_utils, logging, libm.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef SEMANTIC_SEARCH_TEST
static int sem_alloc_counter = 0;
static int sem_alloc_fail_at = -1;
static int sem_realloc_counter = 0;
static int sem_realloc_fail_at = -1;

static char *sem_test_strdup(const char *s)
{
    sem_alloc_counter++;
    if (sem_alloc_counter == sem_alloc_fail_at) return NULL;
    return str_dup(s);
}

static void *sem_test_realloc(void *ptr, size_t size)
{
    sem_realloc_counter++;
    if (sem_realloc_counter == sem_realloc_fail_at) return NULL;
    return realloc(ptr, size);
}

#define str_dup sem_test_strdup
#define realloc sem_test_realloc
#endif

#define MAX_DOCS 256
#define MAX_TOKENS 8192
#define MAX_TOKEN_LEN 128

typedef struct {
    char **documents;
    int doc_count;
    char **all_terms;
    int term_count;
    int **term_freqs;
    int *doc_lengths;
} SearchIndex;

static SearchIndex search_index = {0};

/* Public teardown for the process-wide index: frees every document, term,
 * frequency row, and the backing arrays, leaving the index empty and
 * reusable. Documented in semantic_search.h; called by the tool destroy
 * and the test reset hook. */
void semantic_search_free_index(void)
{
    for (int i = 0; i < search_index.doc_count; i++)
        free(search_index.documents[i]);
    for (int i = 0; i < search_index.term_count; i++)
        free(search_index.all_terms[i]);
    for (int i = 0; i < search_index.doc_count; i++)
        free(search_index.term_freqs[i]);
    free(search_index.documents);
    free(search_index.all_terms);
    free(search_index.term_freqs);
    free(search_index.doc_lengths);
    search_index.documents = NULL;
    search_index.all_terms = NULL;
    search_index.term_freqs = NULL;
    search_index.doc_lengths = NULL;
    search_index.doc_count = 0;
    search_index.term_count = 0;
}

#ifdef SEMANTIC_SEARCH_TEST
void semantic_search_test_set_alloc_fail(int nth_allocation)
{
    sem_alloc_counter = 0;
    sem_alloc_fail_at = nth_allocation;
}

void semantic_search_test_set_realloc_fail(int nth_realloc)
{
    sem_realloc_counter = 0;
    sem_realloc_fail_at = nth_realloc;
}

void semantic_search_test_reset(void)
{
    sem_alloc_counter = 0;
    sem_alloc_fail_at = -1;
    sem_realloc_counter = 0;
    sem_realloc_fail_at = -1;
    semantic_search_free_index();
}
#endif

static void tokenize(const char *text, char tokens[MAX_TOKENS][MAX_TOKEN_LEN], int *count)
{
    *count = 0;
    int t = 0;
    int ci = 0;

    for (const char *p = text; *p && *count < MAX_TOKENS; p++)
    {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        {
            if (ci < MAX_TOKEN_LEN - 1)
            {
                tokens[t][ci++] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            }
        }
        else if (ci > 0)
        {
            tokens[t][ci] = '\0';
            /* Drop tokens under 3 characters: they carry no TF-IDF
             * signal and only dilute the scores. */
            if (strlen(tokens[t]) >= 3)
            {
                t++;
                *count = t;
            }
            ci = 0;
        }
    }

    if (ci > 0)
    {
        tokens[t][ci] = '\0';
        if (strlen(tokens[t]) >= 3) (*count)++;
    }
}

static int find_term(const char *term)
{
    for (int i = 0; i < search_index.term_count; i++)
    {
        if (strcmp(search_index.all_terms[i], term) == 0) return i;
    }
    return -1;
}

static int add_term(const char *term)
{
    int idx = find_term(term);
    if (idx >= 0) return idx;

    if (search_index.term_count >= MAX_TOKENS) return -1;
    char *dup = str_dup(term);
    if (!dup) return -1;
    int t = search_index.term_count;

    for (int i = 0; i < search_index.doc_count; i++)
    {
        /* Resize every doc's frequency row to the live term count so the
         * index stays proportional to actual terms, not MAX_TOKENS per
         * document. Rows that fail to grow stay at their old size, which
         * is exactly the previous term count — at least as large as the
         * committed term count below, so no later index is out of
         * bounds. */
        int *new_freqs = realloc(search_index.term_freqs[i],
                                 sizeof(int) * ((size_t)t + 1));
        if (!new_freqs)
        {
            /* Commit the term only after every row has room for it; a
             * failed realloc must not leave a row one int short while
             * term_count has already grown, or compute_tfidf reads past
             * the row end. */
            free(dup);
            return -1;
        }
        search_index.term_freqs[i] = new_freqs;
        search_index.term_freqs[i][t] = 0;
    }

    search_index.all_terms[t] = dup;
    search_index.term_count = t + 1;
    return t;
}

int semantic_search_index_document(const char *content)
{
    if (!content) return 0; /* tolerated: indexes nothing */
    if (search_index.doc_count >= MAX_DOCS) return -1;

    /* The large backing arrays are allocated on first use so an empty
     * index costs nothing; a failure here leaves the index reusable. */
    if (!search_index.documents)
    {
        search_index.documents = calloc(MAX_DOCS, sizeof(char *));
        if (!search_index.documents) return -1;
    }
    if (!search_index.all_terms)
    {
        search_index.all_terms = calloc(MAX_TOKENS, sizeof(char *));
        if (!search_index.all_terms) return -1;
    }
    if (!search_index.term_freqs)
    {
        search_index.term_freqs = calloc(MAX_DOCS, sizeof(int *));
        if (!search_index.term_freqs) return -1;
    }
    if (!search_index.doc_lengths)
    {
        search_index.doc_lengths = calloc(MAX_DOCS, sizeof(int));
        if (!search_index.doc_lengths) return -1;
    }

    int idx = search_index.doc_count;
    char *doc_dup = str_dup(content);
    if (!doc_dup) return -1;
    search_index.documents[idx] = doc_dup;

    char tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int token_count = 0;
    tokenize(content, tokens, &token_count);

    search_index.term_freqs[idx] = calloc(MAX_TOKENS, sizeof(int));
    if (!search_index.term_freqs[idx])
    {
        free(search_index.documents[idx]);
        search_index.documents[idx] = NULL;
        return -1;
    }

    search_index.doc_lengths[idx] = 0;

    for (int i = 0; i < token_count; i++)
    {
        int term_idx = add_term(tokens[i]);
        if (term_idx >= 0)
        {
            search_index.term_freqs[idx][term_idx]++;
            search_index.doc_lengths[idx]++;
        }
        else
        {
            /* All-or-nothing per the header contract: a failed term must
             * not leave a half-indexed committed document (the old code
             * silently skipped the term and committed the document with a
             * missing term anyway). */
            free(search_index.documents[idx]);
            search_index.documents[idx] = NULL;
            free(search_index.term_freqs[idx]);
            search_index.term_freqs[idx] = NULL;
            return -1;
        }
    }

    search_index.doc_count++;
    return 0;
}

static double compute_tfidf(const int *freqs, int doc_len, int term_idx, int total_docs)
{
    if (doc_len == 0) return 0;
    double tf = (double)freqs[term_idx] / doc_len;

    int docs_with_term = 0;
    for (int i = 0; i < search_index.doc_count; i++)
    {
        if (search_index.term_freqs[i] && search_index.term_freqs[i][term_idx] > 0)
            docs_with_term++;
    }

    double idf = log((double)total_docs / (docs_with_term > 0 ? docs_with_term : 1)) + 1.0;
    return tf * idf;
}

/* Tool vtable entry: TF-IDF scoring over the shared global index; self
 * is unused because the index is process-wide, not per-tool. */
ToolResult *semantic_search_execute(Tool *self, const char *args_json)
{
    (void)self;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *query_json = cJSON_GetObjectItem(args, "query");
    if (!query_json || !cJSON_IsString(query_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'query' argument", "validation_error");
    }

    const char *query_raw = cJSON_GetStringValue(query_json);
    cJSON *top_json = cJSON_GetObjectItem(args, "top_k");
    int top_k = top_json && cJSON_IsNumber(top_json) ? top_json->valueint : 5;

    char *query = str_dup(query_raw);
    cJSON_Delete(args);

    if (!query)
        return tool_result_error("oom", "execution_error");

    if (search_index.doc_count == 0)
    {
        free(query);
        return tool_result_create("(no documents indexed. Use ingest_document first.)");
    }

    char query_tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int query_token_count = 0;
    tokenize(query, query_tokens, &query_token_count);
    free(query);

    if (query_token_count == 0)
        return tool_result_error("query too short", "validation_error");

    /* map from doc_idx -> score */
    double *scores = calloc(search_index.doc_count, sizeof(double));
    int *indices = malloc(sizeof(int) * search_index.doc_count);
    if (!scores || !indices) {
        free(scores);
        free(indices);
        return tool_result_error("oom", "execution_error");
    }

    for (int i = 0; i < search_index.doc_count; i++) indices[i] = i;

    for (int i = 0; i < search_index.doc_count; i++)
    {
        /* term_freqs[i] is NULL for a document whose indexing hit an
         * allocation failure; skip it rather than dereference. */
        if (!search_index.term_freqs[i]) continue;
        for (int t = 0; t < query_token_count; t++)
        {
            int term_idx = find_term(query_tokens[t]);
            if (term_idx >= 0)
            {
                scores[i] += compute_tfidf(search_index.term_freqs[i],
                                           search_index.doc_lengths[i],
                                           term_idx, search_index.doc_count);
            }
        }
    }

    /* sort descending */
    for (int i = 0; i < search_index.doc_count - 1; i++)
    {
        for (int j = i + 1; j < search_index.doc_count; j++)
        {
            if (scores[j] > scores[i])
            {
                double tmp_s = scores[i];
                scores[i] = scores[j];
                scores[j] = tmp_s;
                int tmp_i = indices[i];
                indices[i] = indices[j]; // NOLINT(clang-analyzer-core.uninitialized.Assign)
                indices[j] = tmp_i;
            }
        }
    }

    cJSON *out_arr = cJSON_CreateArray();
    for (int i = 0; i < top_k && i < search_index.doc_count; i++)
    {
        int doc_idx = indices[i]; // NOLINT(clang-analyzer-core.uninitialized.Assign)
        if (scores[i] <= 0) break;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "score", scores[i]);

        const char *doc = search_index.documents[doc_idx];
        size_t dlen = doc ? strlen(doc) : 0;
        size_t snippet_len = dlen < 500 ? dlen : 500;
        char *snippet = malloc(snippet_len + 1);
        if (snippet)
        {
            if (doc) memcpy(snippet, doc, snippet_len);
            snippet[snippet_len] = '\0';
            cJSON_AddStringToObject(item, "snippet", snippet);
            free(snippet);
        }
        cJSON_AddItemToArray(out_arr, item);
    }

    free(scores);
    free(indices);

    char *result = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);

    if (!result) return tool_result_create("(no results)");
    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;
}

/* Tool vtable entry: also releases the process-wide index, so destroying
 * one semantic_search tool empties it for every other user. */
void semantic_search_destroy(Tool *self)
{
    if (!self) return;
    semantic_search_free_index();

    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self);
}

Tool *tool_semantic_search_create(SafetyConfig *safety)
{
    /* A read-only local search needs no safety policy; the pointer is
     * accepted only to match the other tool factories. */
    (void)safety;
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    t->name = str_dup("semantic_search");
    t->description = str_dup("Search indexed documents by semantic similarity using TF-IDF");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"
        "\"top_k\":{\"type\":\"integer\",\"description\":\"Number of results (default 5)\"}"
        "},\"required\":[\"query\"]}"
    );
    t->execute = semantic_search_execute;
    t->destroy = semantic_search_destroy;
    t->ctx = NULL;
    return t;
}
