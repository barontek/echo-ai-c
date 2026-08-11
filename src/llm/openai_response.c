/*
 * openai_response.c - Codex response parsing and HTTP plumbing:
 * buffered-response extraction, credential/header management,
 * 401 recovery, and the model-catalog fetch.
 * Depends on: libcurl, cJSON, OpenSSL, openai_request, logging.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <openssl/crypto.h>

#include "openai.h"
#include "openai_internal.h"
#include "openai_oauth.h"
#include "openai_response.h"
#include "openai_request.h"
#include "openai_stream.h"
#include "../utils/http_client.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"


static void clear_secret(char **value)
{
    if (!value || !*value) return;
    memset(*value, 0, strlen(*value));
    free(*value);
    *value = NULL;
}

void openai_credentials_clear(Credentials *credentials)
{
    if (!credentials) return;
    clear_secret(&credentials->token);
    free(credentials->account);
    credentials->account = NULL;
}

static int valid_header_value(const char *value, size_t maximum)
{
    if (!value || !value[0]) return 0;
    size_t length = strnlen(value, maximum + 1U);
    return length <= maximum && !strchr(value, '\r') && !strchr(value, '\n');
}

static int header_append(struct curl_slist **headers, const char *value)
{
    struct curl_slist *grown = curl_slist_append(*headers, value);
    if (!grown) return -1;
    *headers = grown;
    return 0;
}

void headers_free(struct curl_slist *headers)
{
    for (struct curl_slist *item = headers; item; item = item->next)
        if (item->data && strncmp(item->data, "Authorization: Bearer ", 22) == 0)
            OPENSSL_cleanse(item->data, strlen(item->data));
    curl_slist_free_all(headers);
}

static int build_headers(const Credentials *credentials,
                         struct curl_slist **headers_out)
{
    if (!credentials || !headers_out ||
        !valid_header_value(credentials->token, OPENAI_MAX_TOKEN_BYTES) ||
        (credentials->account &&
         !valid_header_value(credentials->account, OPENAI_MAX_ACCOUNT_BYTES)))
        return -1;
    *headers_out = NULL;
    char authorization[OPENAI_MAX_TOKEN_BYTES + 32U] = {0};
    int written = snprintf(authorization, sizeof(authorization),
                           "Authorization: Bearer %s", credentials->token);
    if (written < 0 || (size_t)written >= sizeof(authorization))
     {
        OPENSSL_cleanse(authorization, sizeof(authorization));
        return -1;
    }
    int header_result = header_append(headers_out, "Content-Type: application/json") == 0 &&
        header_append(headers_out, authorization) == 0 &&
        header_append(headers_out, "originator: echo-ai") == 0 &&
        header_append(headers_out, "User-Agent: echo-ai") == 0;
    OPENSSL_cleanse(authorization, sizeof(authorization));
    if (!header_result)
        goto fail;
    if (credentials->account)
    {
        char account_header[OPENAI_MAX_ACCOUNT_BYTES + 32U] = {0};
        written = snprintf(account_header, sizeof(account_header),
                           "ChatGPT-Account-Id: %s", credentials->account);
        int account_result = written >= 0 && (size_t)written < sizeof(account_header) &&
                             header_append(headers_out, account_header) == 0;
        OPENSSL_cleanse(account_header, sizeof(account_header));
        if (!account_result)
            goto fail;
    }
    return 0;

fail:
    headers_free(*headers_out);
    *headers_out = NULL;
    return -1;
}

int request_setup(CURL *curl, const char *body, int timeout,
                         const Credentials *credentials,
                         struct curl_slist **headers_out)
{
    if (!curl || !body || timeout <= 0 ||
        strnlen(body, OPENAI_MAX_REQUEST_BYTES + 1U) > OPENAI_MAX_REQUEST_BYTES ||
        build_headers(credentials, headers_out) != 0)
        return -1;
    if (curl_easy_setopt(curl, CURLOPT_URL, CODEX_ENDPOINT) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers_out) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK)
    {
        headers_free(*headers_out);
        *headers_out = NULL;
        return -1;
    }
    return 0;
}

int credentials_get(OpenAIOAuth *auth, Credentials *credentials)
{
    if (!credentials ||
        openai_oauth_get_access_token(auth, &credentials->token,
                                      &credentials->account) != 0 ||
        !valid_header_value(credentials->token, OPENAI_MAX_TOKEN_BYTES) ||
        (credentials->account &&
         !valid_header_value(credentials->account, OPENAI_MAX_ACCOUNT_BYTES)))
    {
        openai_credentials_clear(credentials);
        return -1;
    }
    return 0;
}

int credentials_refresh_401(OpenAIOAuth *auth,
                                   Credentials *credentials)
{
    char *access_token = NULL;
    char *account_id = NULL;
    if (!auth || !credentials || !credentials->token ||
        openai_oauth_refresh_after_401(auth, credentials->token,
                                       &access_token, &account_id) !=
            OPENAI_OAUTH_TOKEN_OK)
    {
        clear_secret(&access_token);
        free(account_id);
        return -1;
    }
    if (!valid_header_value(access_token, OPENAI_MAX_TOKEN_BYTES) ||
        (account_id && !valid_header_value(account_id, OPENAI_MAX_ACCOUNT_BYTES)))
    {
        clear_secret(&access_token);
        free(account_id);
        return -1;
    }
    openai_credentials_clear(credentials);
    credentials->token = access_token;
    credentials->account = account_id;
    return 0;
}

static int model_already_added(char **models, size_t count, const char *slug)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(models[i], slug) == 0) return 1;
    return 0;
}

static void log_empty_catalog_diagnostic(const char *body)
{
    cJSON *root = NULL;
    if (!body || parse_bounded_json(body, OPENAI_MAX_MODELS_RESPONSE_BYTES,
                                    &root) != 0 || !cJSON_IsObject(root))
     {
        cJSON_Delete(root);
        return;
    }
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "models");
    int total = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    int list_visible = 0;
    char vis_buf[64] = {0};
    char *write = vis_buf;
    if (cJSON_IsArray(items))
    {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items)
        {
            cJSON *vis_json = cJSON_GetObjectItemCaseSensitive(item, "visibility");
            const char *vis = cJSON_IsString(vis_json) ?
                cJSON_GetStringValue(vis_json) : "<non-string>";
            if (strcmp(vis, "list") == 0) list_visible++;
            if (write - vis_buf < (long)sizeof(vis_buf) - 2L)
            {
                if (write != vis_buf) *write++ = ',';
                int copied = snprintf(write,
                    (size_t)(vis_buf + sizeof(vis_buf) - write), "%s", vis);
                if (copied > 0 &&
                    (size_t)copied < (size_t)(vis_buf + sizeof(vis_buf) - write))
                    write += copied;
            }
        }
    }
    char total_buf[16];
    char list_buf[16];
    snprintf(total_buf, sizeof(total_buf), "%d", total);
    snprintf(list_buf, sizeof(list_buf), "%d", list_visible);
    log_warn("OpenAI Codex model catalog contained no list-visible models",
             "items", total_buf, "list_visible", list_buf,
             "visibility_values", vis_buf, NULL);
    cJSON_Delete(root);
}

int parse_models_response(const char *raw, char ***models_out,
                                 size_t *count_out)
{

    if (!models_out || !count_out) return -1;
    *models_out = NULL;
    *count_out = 0U;
    cJSON *root = NULL;
    if (parse_bounded_json(raw, OPENAI_MAX_MODELS_RESPONSE_BYTES, &root) != 0 ||
        !cJSON_IsObject(root))
     {
        cJSON_Delete(root);
        return -1;
    }
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "models");
    int item_count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : -1;
    if (item_count < 0 || (size_t)item_count > OPENAI_MAX_MODELS)
     {
        cJSON_Delete(root);
        return -1;
    }
    char **models = item_count > 0 ?
        calloc((size_t)item_count, sizeof(*models)) : NULL;
    if (item_count > 0 && !models) {
        cJSON_Delete(root);
        return -1;
    }
    size_t count = 0U;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        cJSON *slug_json = cJSON_GetObjectItemCaseSensitive(item, "slug");
        cJSON *visibility_json = cJSON_GetObjectItemCaseSensitive(item, "visibility");
        const char *slug = cJSON_IsString(slug_json) ?
            cJSON_GetStringValue(slug_json) : NULL;
        const char *visibility = cJSON_IsString(visibility_json) ?
            cJSON_GetStringValue(visibility_json) : NULL;
        if (!cJSON_IsObject(item) || !slug || !slug[0] || !visibility ||
            strnlen(slug, OPENAI_MAX_MODEL_NAME_BYTES + 1U) >
                OPENAI_MAX_MODEL_NAME_BYTES)
            continue;
        if (strcmp(visibility, "list") != 0 ||
            model_already_added(models, count, slug))
            continue;
        models[count] = str_dup(slug);
        if (!models[count]) goto fail;
        count++;
    }
    cJSON_Delete(root);
    if (count == 0U)
    {
        /* Empty catalog: keep the "count 0 means NULL" contract. */
        free(models);
        models = NULL;
    }
    *models_out = models;
    *count_out = count;
    return 0;

fail:
    cJSON_Delete(root);
    openai_models_free(models, count);
    return -1;
}

static int models_request_once(const Credentials *credentials, char **body_out,
                               long *status_out)
{
    *body_out = NULL;
    *status_out = 0L;
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    HttpBuffer body = {.limit = OPENAI_MAX_MODELS_RESPONSE_BYTES};
    char url[512] = {0};
    char version_header[128] = {0};
    int url_len = snprintf(url, sizeof(url), "%s?client_version=%s",
                           CODEX_MODELS_ENDPOINT, CODEX_CLIENT_VERSION);
    int header_len = snprintf(version_header, sizeof(version_header),
                              "version: %s", CODEX_CLIENT_VERSION);
    int setup_ok = curl && url_len > 0 && (size_t)url_len < sizeof(url) &&
        header_len > 0 && (size_t)header_len < sizeof(version_header) &&
        build_headers(credentials, &headers) == 0 &&
        header_append(&headers, version_header) == 0 &&
        curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_buffer_write_cb) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body) == CURLE_OK;
    CURLcode performed = setup_ok ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    if (performed == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out) != CURLE_OK)
        performed = CURLE_GOT_NOTHING;
    headers_free(headers);
    if (curl) curl_easy_cleanup(curl);
    if (performed != CURLE_OK)
    {
        free(body.data);
        return -1;
    }
    *body_out = body.data;
    return 0;
}

static void copy_safe_error_field(char output[OPENAI_ERROR_FIELD_BYTES],
                                  const cJSON *field)
{
    const char *value = cJSON_IsString(field) ? cJSON_GetStringValue(field) : NULL;
    size_t used = 0U;
    if (!value) {
        output[0] = '\0';
        return;
    }
    while (*value && used + 1U < OPENAI_ERROR_FIELD_BYTES)
    {
        unsigned char byte = (unsigned char)*value++;
        output[used++] = (char)(isalnum(byte) || byte == '.' || byte == '_' ||
                                byte == '-' ? byte : '_');
    }
    output[used] = '\0';
}

void log_http_error(const char *operation, long status,
                           const char *body)
{
    char status_text[32] = {0};
    char type[OPENAI_ERROR_FIELD_BYTES] = {0};
    char code[OPENAI_ERROR_FIELD_BYTES] = {0};
    int written = snprintf(status_text, sizeof(status_text), "%ld", status);
    if (written < 0 || (size_t)written >= sizeof(status_text))
        memcpy(status_text, "unknown", sizeof("unknown"));
    cJSON *root = NULL;
    if (body && parse_bounded_json(body, OPENAI_MAX_RESPONSE_BYTES, &root) == 0)
    {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (!cJSON_IsObject(error)) error = root;
        copy_safe_error_field(type,
            cJSON_GetObjectItemCaseSensitive(error, "type"));
        copy_safe_error_field(code,
            cJSON_GetObjectItemCaseSensitive(error, "code"));
    }
    log_error("OpenAI Codex request returned non-2xx", "operation", operation,
              "status", status_text, "error_type", type[0] ? type : "unknown",
              "error_code", code[0] ? code : "unknown", NULL);
    cJSON_Delete(root);
}

int add_response_tool_call(LLMResponse *response, const char *id,
                                  const char *name, const char *arguments,
                                  int *index_out)
{
    if (!response || response->tool_calls_count < 0 ||
        response->tool_calls_count == INT_MAX)
        return -1;
    char *id_copy = str_dup(id ? id : "");
    char *name_copy = str_dup(name ? name : "");
    char *arguments_copy = str_dup(arguments ? arguments : "");
    if (!id_copy || !name_copy || !arguments_copy)
    {
        free(id_copy);
        free(name_copy);
        free(arguments_copy);
        return -1;
    }
    size_t count = (size_t)response->tool_calls_count + 1U;
    if (count > SIZE_MAX / sizeof(*response->tool_calls))
    {
        free(id_copy);
        free(name_copy);
        free(arguments_copy);
        return -1;
    }
    ToolCall *grown = realloc(response->tool_calls,
                              count * sizeof(*response->tool_calls));
    if (!grown)
    {
        free(id_copy);
        free(name_copy);
        free(arguments_copy);
        return -1;
    }
    response->tool_calls = grown;
    ToolCall *call = &grown[response->tool_calls_count];
    memset(call, 0, sizeof(*call));
    call->id = id_copy;
    call->name = name_copy;
    call->arguments = arguments_copy;
    if (index_out) *index_out = response->tool_calls_count;
    response->tool_calls_count++;
    return 0;
}

int response_status_ok(const cJSON *root)
{
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (error && !cJSON_IsNull(error)) return 0;
    if (!status) return 1;
    return cJSON_IsString(status) &&
           strcmp(cJSON_GetStringValue(status), "completed") == 0;
}

#ifdef OPENAI_TEST
#ifdef OPENAI_TEST
static int parse_function_call_item(LLMResponse *response, const cJSON *item)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(item, "arguments");
    if (!valid_nonempty_string(id) || !valid_nonempty_string(name) ||
        !cJSON_IsString(arguments))
        return -1;
    return add_response_tool_call(response, cJSON_GetStringValue(id),
                                  cJSON_GetStringValue(name),
                                  cJSON_GetStringValue(arguments), NULL);
}

static int parse_message_item(LLMResponse *response, const cJSON *item)
{
    cJSON *phase = cJSON_GetObjectItemCaseSensitive(item, "phase");
    if (phase)
    {
        if (!valid_nonempty_string(phase)) return -1;
        char *copy = str_dup(cJSON_GetStringValue(phase));
        if (!copy) return -1;
        free(response->phase);
        response->phase = copy;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
    if (!cJSON_IsArray(content)) return -1;
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
        if (!valid_nonempty_string(type)) return -1;
        const char *type_value = cJSON_GetStringValue(type);
        cJSON *text = strcmp(type_value, "refusal") == 0
                          ? cJSON_GetObjectItemCaseSensitive(part, "refusal")
                          : cJSON_GetObjectItemCaseSensitive(part, "text");
        if (strcmp(type_value, "output_text") != 0 &&
            strcmp(type_value, "refusal") != 0)
            continue;
        if (!cJSON_IsString(text) ||
            str_append(&response->content, cJSON_GetStringValue(text)) != 0)
            return -1;
    }
    return 0;
}
#endif
#endif

#ifdef OPENAI_TEST
LLMResponse *parse_response(const char *raw)
{
    cJSON *root = NULL;
    if (parse_bounded_json(raw, OPENAI_MAX_RESPONSE_BYTES, &root) != 0 ||
        !cJSON_IsObject(root) || !response_status_ok(root))
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    if (!cJSON_IsArray(output)) {
        cJSON_Delete(root);
        return NULL;
    }
    LLMResponse *response = llm_response_create();
    if (!response) {
        cJSON_Delete(root);
        return NULL;
    }
    char *reasoning_text = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, output)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!valid_nonempty_string(type)) goto fail;
        const char *value = cJSON_GetStringValue(type);
        if (strcmp(value, "message") == 0)
        {
            if (parse_message_item(response, item) != 0) goto fail;
        }
        else if (strcmp(value, "function_call") == 0)
        {
            if (parse_function_call_item(response, item) != 0) goto fail;
        }
        else if (strcmp(value, "reasoning") == 0)
        {
            char *summary = reasoning_summary_join(item);
            if (summary)
            {
                if ((reasoning_text && str_append(&reasoning_text, "\n") != 0) ||
                    str_append(&reasoning_text, summary) != 0)
                 {
                    free(summary);
                    goto fail;
                }
                free(summary);
            }
        }
    }
    if (!response->content && str_append(&response->content, "") != 0) goto fail;
    if (reasoning_text && reasoning_text[0])
    {
        char *tagged = NULL;
        if (asprintf(&tagged, "<think>\n%s\n</think>\n\n%s", reasoning_text,
                     response->content ? response->content : "") < 0)
         {
            free(reasoning_text);
            goto fail;
        }
        free(response->content);
        response->content = tagged;
        response->thinking = reasoning_text; /* ownership transferred */
    }
    else
    {
        free(reasoning_text);
    }
    cJSON_Delete(root);
    return response;

fail:
    cJSON_Delete(root);
    llm_response_free(response);
    return NULL;
}
#endif


int openai_models_fetch_alloc(OpenAIOAuth *auth, char ***models_out,
                              size_t *count_out)
{
    if (!auth || !models_out || !count_out) return -1;
    *models_out = NULL;
    *count_out = 0U;
    Credentials credentials = {0};
    if (credentials_get(auth, &credentials) != 0) return OPENAI_MODELS_UNAVAILABLE;
    int result = OPENAI_MODELS_UNAVAILABLE;
    int denied = 0;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        char *body = NULL;
        long status = 0L;
        if (models_request_once(&credentials, &body, &status) != 0)
         {
            log_error("OpenAI Codex models request failed", NULL);
            free(body);
            break;
        }
        if (status == 401L && attempt == 0 &&
            credentials_refresh_401(auth, &credentials) == 0)
         {
            free(body);
            continue;
        }
        if (status < 200L || status >= 300L)
        {
            log_http_error("models", status, body);
            if (status >= 400L && status < 500L) denied = 1;
        }
        else
        {
            result = parse_models_response(body, models_out, count_out);
            if (result == 0 && *count_out == 0U)
                log_empty_catalog_diagnostic(body);
            if (result != 0 || *count_out == 0U)
            {
                char snippet[257] = {0};
                if (body) memcpy(snippet, body, sizeof(snippet) - 1U);
                log_debug("OpenAI Codex models response", "body", snippet, NULL);
            }
        }
        free(body);
        break;
    }
    openai_credentials_clear(&credentials);
    return denied ? OPENAI_MODELS_DENIED : result;
}

void openai_models_free(char **models, size_t count)
{
    if (!models) return;
    for (size_t i = 0; i < count; i++) free(models[i]);
    free(models);
}
