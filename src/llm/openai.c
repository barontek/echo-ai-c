/*
 * openai.c - ChatGPT Codex provider: provider vtable, streaming
 * transport, and test hooks. Request building, stream parsing, and
 * response handling live in openai_request/stream/response units;
 * shared state in openai_internal.h.
 * Depends on: libcurl, openai_oauth, provider types.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "openai.h"
#include "openai_internal.h"
#include "openai_request.h"
#include "openai_stream.h"
#include "openai_response.h"
#include "../agent/message.h"
#include "../utils/http_client.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

typedef struct {
    OpenAIOAuth *auth;
    char *effort; /* owned; NULL = API default ("low"/"medium"/"high"/"xhigh"/"max"/"none") */
} OpenAICtx;

typedef struct {
    CURL *curl;
    long status;
    HttpBuffer error_body;
    StreamParser parser;
} LiveContext;


static size_t header_write_cb(void *ptr, size_t size, size_t nmemb,
                              void *userdata)
{
    LiveContext *context = userdata;
    if (size != 0U && nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    const char *bytes = ptr;
    if (total >= 12U && total < 64U && memcmp(bytes, "HTTP/", 5U) == 0)
    {
        char line[64] = {0};
        memcpy(line, bytes, total);
        char *space = strchr(line, ' ');
        if (!space) return 0;
        char *end = NULL;
        long status = strtol(space + 1, &end, 10);
        if (end == space + 1 || status < 100L || status > 599L) return 0;
        context->status = status;
    }
    return total;
}

static size_t live_write_cb(void *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
    LiveContext *context = userdata;
    if (size != 0U && nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    if (context->status < 200L || context->status >= 300L)
        return http_buffer_append(&context->error_body, ptr, total) == 0 ? total : 0;
    return stream_feed(&context->parser, ptr, total) == 0 ? total : 0;
}

static LLMResponse *perform_stream(OpenAIOAuth *auth, const char *body,
                                  int timeout,
                                  void (*on_chunk)(const char *, void *),
                                  void *userdata)
{
    Credentials credentials = {0};
    if (credentials_get(auth, &credentials) != 0) return NULL;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        CURL *curl = curl_easy_init();
        struct curl_slist *headers = NULL;
        LLMResponse *response = llm_response_create();
        LiveContext live = {.curl = curl,
                            .error_body = {.limit = OPENAI_MAX_RESPONSE_BYTES}};
        live.parser.response = response;
        live.parser.event_data.limit = OPENAI_MAX_SSE_EVENT_BYTES;
        live.parser.on_chunk = on_chunk;
        live.parser.userdata = userdata;
        CURLcode performed = CURLE_FAILED_INIT;
        if (curl && response &&
            request_setup(curl, body, timeout, &credentials, &headers) == 0 &&
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write_cb) == CURLE_OK &&
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &live) == CURLE_OK &&
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, live_write_cb) == CURLE_OK &&
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &live) == CURLE_OK)
            performed = curl_easy_perform(curl);
        long status = live.status;
        if (performed == CURLE_OK &&
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
            performed = CURLE_GOT_NOTHING;
        headers_free(headers);
        if (curl) curl_easy_cleanup(curl);
        int retry = performed == CURLE_OK && status == 401L && attempt == 0;
        if (retry && credentials_refresh_401(auth, &credentials) == 0)
        {
            stream_parser_cleanup(&live.parser);
            free(live.error_body.data);
            llm_response_free(response);
            continue;
        }
        if (performed != CURLE_OK)
            log_error("OpenAI Codex request failed", "operation", "stream",
                      "error", curl_easy_strerror(performed), NULL);
        else if (status < 200L || status >= 300L)
            log_http_error("stream", status, live.error_body.data);
        else if (stream_finish(&live.parser) != 0)
            log_error("OpenAI Codex stream validation failed", "operation",
                      "stream", NULL);
        else
        {
            stream_parser_cleanup(&live.parser);
            free(live.error_body.data);
            openai_credentials_clear(&credentials);
            return response;
        }
        stream_parser_cleanup(&live.parser);
        free(live.error_body.data);
        llm_response_free(response);
        break;
    }
    openai_credentials_clear(&credentials);
    return NULL;
}

static LLMResponse *openai_chat(LLMProvider *provider, Message *messages,
                                int count, const char *model,
                                double temperature, int timeout,
                                const char *tools_json)
{
    if (!provider || !provider->ctx || timeout <= 0) return NULL;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, temperature, 1,
                                    tools_json, NULL, context->effort);
    if (!body)
    {
        log_error("OpenAI Codex request conversion failed", "operation",
                  "buffered", NULL);
        return NULL;
    }
    LLMResponse *response = perform_stream(context->auth, body, timeout, NULL, NULL);
    free(body);
    return response;
}

static LLMResponse *openai_stream(LLMProvider *provider, Message *messages,
                                  int count, const char *model,
                                  double temperature, int timeout,
                                  void (*on_chunk)(const char *, void *),
                                  void *userdata, const char *tools_json)
{
    if (!provider || !provider->ctx || timeout <= 0) return NULL;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, temperature, 1,
                                    tools_json, NULL, context->effort);
    if (!body)
    {
        log_error("OpenAI Codex request conversion failed", "operation",
                  "stream", NULL);
        return NULL;
    }
    LLMResponse *response = perform_stream(context->auth, body, timeout,
                                           on_chunk, userdata);
    free(body);
    return response;
}

static LLMResponse *openai_structured(LLMProvider *provider, Message *messages,
                                      int count, const char *model,
                                      double temperature, int timeout,
                                      const char *json_schema)
{
    if (!provider || !provider->ctx || timeout <= 0 || !json_schema ||
        !json_schema[0])
        return NULL;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, temperature, 1,
                                    NULL, json_schema, context->effort);
    if (!body)
    {
        log_error("OpenAI Codex request conversion failed", "operation",
                  "structured", NULL);
        return NULL;
    }
    LLMResponse *response = perform_stream(context->auth, body, timeout, NULL, NULL);
    free(body);
    return response;
}

static void openai_destroy(LLMProvider *provider)
{
    if (!provider) return;
    OpenAICtx *context = provider->ctx;
    if (context) free(context->effort);
    free(context);
    free(provider);
}

LLMProvider *openai_provider_create(const char *base_url, const char *api_token,
                                    const char *effort, OpenAIOAuth *auth)
{
    (void)base_url;
    (void)api_token;
    if (!auth) return NULL;
    if (effort && !openai_reasoning_effort_valid(effort))
    {
        log_error("OpenAI provider rejected invalid reasoning effort",
                  "effort", effort ? effort : "", NULL);
        return NULL;
    }
    LLMProvider *provider = calloc(1, sizeof(*provider));
    OpenAICtx *context = calloc(1, sizeof(*context));
    char *effort_copy = effort && effort[0] ? str_dup(effort) : NULL;
    if (!provider || !context || (effort && effort[0] && !effort_copy))
    {
        free(provider);
        free(context);
        free(effort_copy);
        return NULL;
    }
    context->auth = auth;
    context->effort = effort_copy;
    provider->chat = openai_chat;
    provider->chat_streaming = openai_stream;
    provider->extract_structured = openai_structured;
    provider->destroy = openai_destroy;
    provider->ctx = context;
    return provider;
}

#ifdef OPENAI_TEST
char *openai_test_build_request_body_alloc(Message *messages, int count,
                                     const char *model, double temperature,
                                     int stream, const char *tools_json,
                                     const char *json_schema,
                                     const char *effort)
{
    return build_request_body(messages, count, model, temperature, stream,
                              tools_json, json_schema, effort);
}

LLMResponse *openai_test_parse_response_alloc(const char *raw)
{
    return parse_response(raw);
}

LLMResponse *openai_test_stream_fragments_alloc(
    const char **fragments, const size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata)
{
    if (!fragments || count < 0) return NULL;
    LLMResponse *response = llm_response_create();
    if (!response) return NULL;
    StreamParser parser = {.response = response,
                           .event_data = {.limit = OPENAI_MAX_SSE_EVENT_BYTES},
                           .on_chunk = on_chunk,
                           .userdata = userdata};
    for (int i = 0; i < count; i++)
    {
        if (!fragments[i] ||
            stream_feed(&parser, fragments[i],
                        lengths ? lengths[i] : strlen(fragments[i])) != 0)
            goto fail;
    }
    if (stream_finish(&parser) != 0) goto fail;
    stream_parser_cleanup(&parser);
    return response;

fail:
    stream_parser_cleanup(&parser);
    llm_response_free(response);
    return NULL;
}

int openai_test_request_metadata(const char *token, const char *account,
                                 const char *body, int timeout,
                                 char **url_out, char **headers_out,
                                 long *timeout_out)
{
    if (!url_out || !headers_out || !timeout_out) return -1;
    *url_out = NULL;
    *headers_out = NULL;
    *timeout_out = 0L;
    CURL *curl = curl_easy_init();
    Credentials credentials = {.token = (char *)token,
                               .account = (char *)account};
    struct curl_slist *headers = NULL;
    if (!curl || request_setup(curl, body, timeout, &credentials, &headers) != 0)
    {
        if (curl) curl_easy_cleanup(curl);
        return -1;
    }
    HttpBuffer joined = {.limit = OPENAI_MAX_TOKEN_BYTES + OPENAI_MAX_ACCOUNT_BYTES + 256U};
    for (struct curl_slist *header = headers; header; header = header->next)
        if (http_buffer_append(&joined, header->data, strlen(header->data)) != 0 ||
            http_buffer_append(&joined, "\n", 1U) != 0)
        {
            free(joined.data);
            headers_free(headers);
            curl_easy_cleanup(curl);
            return -1;
        }
    *url_out = str_dup(CODEX_ENDPOINT);
    if (!*url_out)
    {
        free(joined.data);
        headers_free(headers);
        curl_easy_cleanup(curl);
        return -1;
    }
    *headers_out = joined.data;
    *timeout_out = (long)timeout;
    headers_free(headers);
    curl_easy_cleanup(curl);
    return 0;
}

int openai_test_refresh_after_401(OpenAIOAuth *auth,
                                  const char *rejected_token,
                                  char **access_token, char **account_id)
{
    if (!rejected_token || !access_token || !account_id) return -1;
    *access_token = NULL;
    *account_id = NULL;
    Credentials credentials = {.token = str_dup(rejected_token)};
    if (!credentials.token || credentials_refresh_401(auth, &credentials) != 0)
    {
        openai_credentials_clear(&credentials);
        return -1;
    }
    *access_token = credentials.token;
    *account_id = credentials.account;
    credentials.token = NULL;
    credentials.account = NULL;
    return 0;
}

int openai_test_parse_models(const char *raw, char ***models_out,
                             size_t *count_out)
{
    return parse_models_response(raw, models_out, count_out);
}
#endif
