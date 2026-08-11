/*
 * test_ollama_fixture.c - shared fixtures for the ollama test
 * binaries. Split from test_ollama.c (2026-08 file-length
 * compliance).
 */

#include "test_ollama_fixture.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <check.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "../../src/agent/message.h"
#include "../../src/llm/provider.h"
#include "../../src/utils/string_utils.h"

/* test_ollama - unit tests for ollama. Depends on: check, the module under test. */
/* ---- Mirror structs from ollama.c (must stay in sync) ---- */

/* ---- Wrappers exposed under OLLAMA_TEST in ollama.c ---- */

/* ---- Curl stub globals (defined in ollama.c under OLLAMA_TEST) ---- */

/* ---- Public create ---- */

/* ---- Reset curl stub state ---- */
void curl_stubs_reset(void)
{
    ollama_test_captured_url = NULL;
    ollama_test_captured_body = NULL;
    ollama_test_curl_init_fails = 0;
    ollama_test_curl_result = CURLE_OK;
}

/* ---- Shared chunk-collecting callback for forward_chunk tests ---- */

void collect_chunk(const char *chunk, void *userdata)
{
    ChunkCollector *cc = userdata;
    size_t clen = strlen(chunk);
    size_t needed = cc->len + clen + 1;
    if (needed > cc->cap)
    {
        cc->cap = needed * 2;
        char *new_data = realloc(cc->collected, cc->cap);
        if (!new_data) return;
        cc->collected = new_data;
    }
    memcpy(cc->collected + cc->len, chunk, clen + 1);
    cc->len += clen;
}

void chunk_collector_free(ChunkCollector *cc)
{
    free(cc->collected);
    memset(cc, 0, sizeof(*cc));
}

/* ---- Helper: create messages array ---- */
Message *make_messages(const char *role, const char *content)
{
    Message *m = calloc(1, sizeof(Message));
    if (m)
    {
        m->role = str_dup(role);
        m->content = str_dup(content);
    }
    return m;
}

void free_messages(Message *msgs, int count)
{
    for (int i = 0; i < count; i++)
    {
        free(msgs[i].role);
        free(msgs[i].content);
    }
    free(msgs);
}
