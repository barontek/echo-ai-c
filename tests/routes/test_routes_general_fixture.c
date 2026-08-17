/*
 * test_routes_general_fixture.c - shared stubs and fixtures for the
 * routes_general test binaries (general/models): curl, provider,
 * metrics, and session stubs plus capture state. Split from
 * test_routes_general.c (2026-08 file-length compliance). Depends on:
 * check, routes_general.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <check.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <cjson/cJSON.h>

#include "test_routes_general_fixture.h"

/* test_routes_general - unit tests for routes general. Depends on: check, the module under test. */
extern OpenAIOAuthState openai_oauth_stub_state;

/* ---------------------------------------------------------------------------
 * Stub state
 * --------------------------------------------------------------------------- */

int stub_has_valid_token = 1;
int stub_ct_undo_result = -1;
int stub_ct_redo_result = -1;
char *stub_metrics_body = NULL;
int stub_curl_init_nonnull = 0;
int stub_curl_perform_code = 0; /* CURLE_OK */
void *captured_writedata = NULL;
size_t (*captured_writefunc)(void *, size_t, size_t, void *) = NULL;
const char *stub_models_json = NULL;
char captured_curl_url[256] = {0};
char captured_auth_header[256] = {0};
int captured_status = 0;
char *captured_body = NULL;
int stub_openai_models_result = -1;
const char *stub_openai_models[8] = {NULL};
size_t stub_openai_models_count = 0U;


void reset_capture(void)
{
    captured_status = 0;
    memset(captured_auth_header, 0, sizeof(captured_auth_header));
    free(captured_body);
    captured_body = NULL;
}

void reset_stubs(void)
{
    stub_has_valid_token = 1;
    stub_ct_undo_result = -1;
    stub_ct_redo_result = -1;
    stub_metrics_body = NULL;
    stub_curl_init_nonnull = 0;
    stub_curl_perform_code = 0; /* CURLE_OK */
    captured_writedata = NULL;
    captured_writefunc = NULL;
    stub_models_json = NULL;
    stub_openai_models_result = -1;
    stub_openai_models_count = 0U;
    for (size_t i = 0; i < 8U; i++) stub_openai_models[i] = NULL;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_OUT;
    memset(captured_curl_url, 0, sizeof(captured_curl_url));
    reset_capture();
}

/* dummy storage for opaque Metrics pointer */
int dummy_metrics_ptr = 0;

/* ---------------------------------------------------------------------------
 * Stub server functions
 * --------------------------------------------------------------------------- */

int server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client; (void)content_type;
    captured_status = status;
    free(captured_body);
    captured_body = body ? str_dup(body) : NULL;
    return 0;
}

int server_response_json(Client *client, int status, const char *json)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    captured_body = json ? str_dup(json) : NULL;
    return 0;
}

int server_response_error(Client *client, int status, const char *msg)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg ? msg : "");
    captured_body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return 0;
}

void client_close(Client *client) { (void)client; }
int server_sse_write(Client *client, const char *data)
 {
    (void)client;
    (void)data;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Stub middleware
 * --------------------------------------------------------------------------- */

int middleware_has_valid_token(const char *headers, const char *token)
{
    (void)headers; (void)token;
    return stub_has_valid_token;
}

/* ---------------------------------------------------------------------------
 * Stub metrics / change_tracker / session_manager
 * --------------------------------------------------------------------------- */

char *metrics_render_new(Metrics *m)
{
    (void)m;
    return stub_metrics_body ? str_dup(stub_metrics_body) : NULL;
}

int ct_undo(ChangeTracker *ct) { (void)ct; return stub_ct_undo_result; }
int ct_redo(ChangeTracker *ct) { (void)ct; return stub_ct_redo_result; }

SessionManager *session_manager_create(const char *d, const char *p)
 {
    (void)d;
    (void)p;
    return NULL;
}

SessionList *session_manager_list_sessions(SessionManager *sm) { (void)sm; return NULL; }
void session_list_free(SessionList *l) { (void)l; }

int openai_models_fetch_alloc(OpenAIOAuth *auth, char ***models_out,
                              size_t *count_out)
{
    (void)auth;
    if (!models_out || !count_out) return -1;
    *models_out = NULL;
    *count_out = 0U;
    if (stub_openai_models_result != 0) return stub_openai_models_result;
    char **models = stub_openai_models_count > 0U ?
        calloc(stub_openai_models_count, sizeof(*models)) : NULL;
    if (stub_openai_models_count > 0U && !models) return -1;
    for (size_t i = 0; i < stub_openai_models_count; i++)
    {
        models[i] = str_dup(stub_openai_models[i]);
        if (!models[i])
        {
            for (size_t j = 0; j < i; j++) free(models[j]);
            free(models);
            return -1;
        }
    }
    *models_out = models;
    *count_out = stub_openai_models_count;
    return 0;
}

void openai_models_free(char **models, size_t count)
{
    for (size_t i = 0; i < count; i++) free(models[i]);
    free(models);
}

/* ---------------------------------------------------------------------------
 * Stub logging
 * --------------------------------------------------------------------------- */

void log_init(void) {}
void log_cleanup(void) {}
void log_set_level(int l) { (void)l; }
void log_msg(int level, const char *file, int line, const char *fmt, ...)
 {
    (void)level;
    (void)file;
    (void)line;
    (void)fmt;
}

/* ---------------------------------------------------------------------------
 * Stub curl — with configurable init/perform for handle_models coverage
 * --------------------------------------------------------------------------- */


CURL *curl_easy_init(void)
{
    return stub_curl_init_nonnull ? (CURL *)&stub_curl_init_nonnull : NULL;
    return 0;
}

CURLcode curl_easy_setopt(CURL *c, int option, ...)
{
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_WRITEFUNCTION)
        captured_writefunc = va_arg(args, size_t (*)(void *, size_t, size_t, void *));
    else if (option == CURLOPT_WRITEDATA)
        captured_writedata = va_arg(args, void *);
    else if (option == CURLOPT_URL)
    {
        const char *u = va_arg(args, const char *);
        snprintf(captured_curl_url, sizeof(captured_curl_url), "%s", u);
    }
    else if (option == CURLOPT_HTTPHEADER)
    {
        struct curl_slist *headers = va_arg(args, struct curl_slist *);
        if (headers && headers->data)
            snprintf(captured_auth_header, sizeof(captured_auth_header), "%s", headers->data);
    }
    va_end(args);
    (void)c;
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *c)
{
    (void)c;
    if (captured_writefunc && captured_writedata && stub_models_json)
        captured_writefunc((void *)stub_models_json, 1,
                           strlen(stub_models_json), captured_writedata);
    return stub_curl_perform_code;
}

void curl_easy_cleanup(CURL *c) { (void)c; }

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
    struct curl_slist *item = calloc(1, sizeof(*item));
    if (!item) return NULL;
    item->data = data ? str_dup(data) : NULL;
    if (data && !item->data) {
        free(item);
        return NULL;
    }
    if (!list) return item;
    struct curl_slist *tail = list;
    while (tail->next) tail = tail->next;
    tail->next = item;
    return list;
}

void curl_slist_free_all(struct curl_slist *list)
{
    while (list)
    {
        struct curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}

/* ---------------------------------------------------------------------------
 * Stub provider list (factory.c is not linked into this test binary)
 * --------------------------------------------------------------------------- */

static const char *const stub_provider_names[] = {
    "ollama", "openai", "openai_compatible", "opencode_zen",
};

const char *const *provider_names_available(int *count)
{
    *count = (int)(sizeof(stub_provider_names) / sizeof(stub_provider_names[0]));
    return stub_provider_names;
}

/* Mirrors factory.c's capability map (factory.c is not linked here); the
 * real function is covered in test_factory.c. */
const char *provider_default_base_url(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "ollama") == 0) return "http://localhost:11434";
    if (strcmp(name, "openai") == 0)
        return "https://chatgpt.com/backend-api/codex/responses";
    if (strcmp(name, "openai_compatible") == 0)
        return "http://localhost:1234";
    if (strcmp(name, "opencode_zen") == 0)
        return "https://opencode.ai/zen/v1";
    if (strcmp(name, "opencode_go") == 0)
        return "https://opencode.ai/zen/go/v1";
    return NULL;
}

int provider_supports_effort(const char *name)
{
    if (!name) return 0;
    return strcmp(name, "openai") == 0 ||
           strcmp(name, "openai_compatible") == 0 ||
           strcmp(name, "ollama") == 0 ||
           strcmp(name, "opencode_zen") == 0;
}

static const char *const stub_openai_effort_options[] = {
    "low", "medium", "high", "xhigh", "max", "none", NULL,
};

static const char *const stub_other_effort_options[] = {
    "low", "medium", "high", "max", "none", NULL,
};

const char *const *provider_effort_options(const char *name)
{
    if (name && strcmp(name, "openai") == 0)
        return stub_openai_effort_options;
    if (name && (strcmp(name, "openai_compatible") == 0 ||
                 strcmp(name, "ollama") == 0 ||
                 strcmp(name, "opencode_zen") == 0))
        return stub_other_effort_options;
    return NULL;
}
void setup(void) { reset_stubs(); }
void teardown(void) { reset_stubs(); }
