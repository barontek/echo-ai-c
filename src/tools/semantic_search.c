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

static char *sem_test_strdup(const char *s)
{
    sem_alloc_counter++;
    if (sem_alloc_counter == sem_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup sem_test_strdup
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

#ifdef SEMANTIC_SEARCH_TEST
void semantic_search_test_set_alloc_fail(int nth_allocation)
{
    sem_alloc_counter = 0;
    sem_alloc_fail_at = nth_allocation;
}

void semantic_search_test_reset(void)
{
    for (int i = 0; i < search_index.doc_count; i++) {
        free(search_index.documents[i]);
        free(search_index.term_freqs[i]);
    }
    for (int i = 0; i < search_index.term_count; i++)
        free(search_index.all_terms[i]);
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
    int t = search_index.term_count++;
    search_index.all_terms[t] = dup;

    for (int i = 0; i < search_index.doc_count; i++)
    {
        int *new_freqs = realloc(search_index.term_freqs[i], sizeof(int) * search_index.term_count);
        if (new_freqs)
        {
            search_index.term_freqs[i] = new_freqs;
            search_index.term_freqs[i][t] = 0;
        }
    }

    return t;
}

void semantic_search_index_document(const char *content)
{
    if (search_index.doc_count >= MAX_DOCS) return;

    if (!search_index.documents)
    {
        search_index.documents = calloc(MAX_DOCS, sizeof(char *));
        if (!search_index.documents) return;
    }
    if (!search_index.all_terms)
    {
        search_index.all_terms = calloc(MAX_TOKENS, sizeof(char *));
        if (!search_index.all_terms) return;
    }
    if (!search_index.term_freqs)
    {
        search_index.term_freqs = calloc(MAX_DOCS, sizeof(int *));
        if (!search_index.term_freqs) return;
    }
    if (!search_index.doc_lengths)
    {
        search_index.doc_lengths = calloc(MAX_DOCS, sizeof(int));
        if (!search_index.doc_lengths) return;
    }

    int idx = search_index.doc_count;
    char *doc_dup = str_dup(content);
    if (!doc_dup) return;
    search_index.documents[idx] = doc_dup;

    char tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int token_count = 0;
    tokenize(content, tokens, &token_count);

    search_index.term_freqs[idx] = calloc(MAX_TOKENS, sizeof(int));
    if (!search_index.term_freqs[idx])
    {
        free(search_index.documents[idx]);
        search_index.documents[idx] = NULL;
        return;
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
    }

    search_index.doc_count++;
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
    if (!scores || !indices) { free(scores); free(indices); return tool_result_error("oom", "execution_error"); }

    for (int i = 0; i < search_index.doc_count; i++) indices[i] = i;

    for (int i = 0; i < search_index.doc_count; i++)
    {
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
                indices[i] = indices[j];
                indices[j] = tmp_i;
            }
        }
    }

    cJSON *out_arr = cJSON_CreateArray();
    for (int i = 0; i < top_k && i < search_index.doc_count; i++)
    {
        int doc_idx = indices[i];
        if (scores[i] <= 0) break;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "score", scores[i]);

        const char *doc = search_index.documents[doc_idx];
        size_t dlen = doc ? strlen(doc) : 0;
        size_t snippet_len = dlen < 500 ? dlen : 500;
        char *snippet = malloc(snippet_len + 1);
        if (snippet)
        {
            memcpy(snippet, doc, snippet_len);
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

void semantic_search_destroy(Tool *self)
{
    if (!self) return;
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

    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self);
}

Tool *tool_semantic_search_create(SafetyConfig *safety)
{
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
