#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <check.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <cjson/cJSON.h>

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_general.h"
#include "../src/llm/openai.h"
#include "../src/config/config.h"
#include "../src/utils/string_utils.h"

extern OpenAIOAuthState openai_oauth_stub_state;

/* ---------------------------------------------------------------------------
 * Stub state
 * --------------------------------------------------------------------------- */

static int stub_has_valid_token = 1;
static int stub_ct_undo_result = -1;
static int stub_ct_redo_result = -1;
static char *stub_metrics_body = NULL;
static int stub_curl_init_nonnull = 0;
static int stub_curl_perform_code = 0; /* CURLE_OK */
static void *captured_writedata = NULL;
static size_t (*captured_writefunc)(void *, size_t, size_t, void *) = NULL;
static const char *stub_models_json = NULL;
static char captured_curl_url[256] = {0};
static char captured_auth_header[256] = {0};
static int captured_status = 0;
static char *captured_body = NULL;
static int stub_openai_models_result = -1;
static const char *stub_openai_models[8] = {NULL};
static size_t stub_openai_models_count = 0U;

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

static void reset_capture(void)
{
    captured_status = 0;
    memset(captured_auth_header, 0, sizeof(captured_auth_header));
    free(captured_body);
    captured_body = NULL;
}

static void reset_stubs(void)
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
static int dummy_metrics_ptr = 0;

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
{ (void)client; (void)data; return 0; }

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
{ (void)d; (void)p; return NULL; }

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
{ (void)level; (void)file; (void)line; (void)fmt; }

/* ---------------------------------------------------------------------------
 * Stub curl — with configurable init/perform for handle_models coverage
 * --------------------------------------------------------------------------- */

typedef void CURL;
typedef int CURLcode;
enum { CURLE_OK = 0, CURLE_UNKNOWN_OPTION = 1, CURLE_URL_MALFORMAT = 3,
       CURLOPT_URL = 10002, CURLOPT_TIMEOUT = 13,
       CURLOPT_WRITEFUNCTION = 20011, CURLOPT_WRITEDATA = 10001,
       CURLOPT_HTTPHEADER = 10023 };

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
    if (data && !item->data) { free(item); return NULL; }
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

/* ---------------------------------------------------------------------------
 * handle_health
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_health_ok)
{
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_health(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"status\":\"ok\""));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_providers
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_providers_lists_available_providers)
{
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_providers(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"providers\":["));
    ck_assert(strstr(captured_body, "\"opencode_zen\""));
    ck_assert(strstr(captured_body, "\"ollama\""));
    ck_assert(strstr(captured_body, "\"effort_supported\":[\"ollama\",\"openai\",\"openai_compatible\",\"opencode_zen\"]"));
    ck_assert(strstr(captured_body, "\"effort_options\":{\"ollama\":[\"low\",\"medium\",\"high\",\"max\",\"none\"],\"openai\":[\"low\",\"medium\",\"high\",\"xhigh\",\"max\",\"none\"],\"openai_compatible\":[\"low\",\"medium\",\"high\",\"max\",\"none\"],\"opencode_zen\":[\"low\",\"medium\",\"high\",\"max\",\"none\"]}"));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_status
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_status_locked)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":true"));
    ck_assert(strstr(captured_body, "\"needs_setup\":false"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_status_setup)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":false"));
    ck_assert(strstr(captured_body, "\"needs_setup\":true"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_status_unlocked_valid_token)
{
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    stub_has_valid_token = 1;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":false"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_status_unlocked_bad_token)
{
    /* token is set but middleware rejects it → locked=1 */
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    stub_has_valid_token = 0;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":true"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_status_noop_token_unlocked_without_header)
{
    /* session-management-disabled mode ("noop" token): the FE must never
     * see locked=1, or it is stuck on the unlock screen forever. */
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "noop";
    stub_has_valid_token = 0;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":false"));
    ck_assert(strstr(captured_body, "\"needs_setup\":false"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_status_session_enabled)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.sm = &sm;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert(strstr(captured_body, "\"session_enabled\":true"));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_config
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_config_no_agent)
{
    ServerContext ctx = {0};
    ctx.agent = NULL;
    HTTPRequest req = {0};

    handle_config(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"provider\":\"ollama\""));
    ck_assert(strstr(captured_body, "\"temperature\":0.7"));
    ck_assert(strstr(captured_body, "\"max_iterations\":50"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_config_with_agent)
{
    Agent agent = {0};
    agent.model = "deepseek";
    agent.temperature = 0.3;
    agent.max_iterations = 25;

    ServerContext ctx = {0};
    ctx.agent = &agent;
    HTTPRequest req = {0};

    handle_config(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"provider\":\"ollama\""));
    ck_assert(strstr(captured_body, "\"model\":\"deepseek\""));
    ck_assert(strstr(captured_body, "\"temperature\":0.3"));
    ck_assert(strstr(captured_body, "\"max_iterations\":25"));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_metrics
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_metrics_no_metrics)
{
    ServerContext ctx = {0};
    ctx.metrics = NULL;
    HTTPRequest req = {0};

    handle_metrics(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_metrics_success)
{
    stub_metrics_body = "echo_requests 42";

    ServerContext ctx = {0};
    ctx.metrics = (Metrics *)&dummy_metrics_ptr;
    HTTPRequest req = {0};

    handle_metrics(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "echo_requests 42"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_metrics_render_null)
{
    stub_metrics_body = NULL;

    ServerContext ctx = {0};
    ctx.metrics = (Metrics *)&dummy_metrics_ptr;
    HTTPRequest req = {0};

    handle_metrics(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_undo
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_undo_no_tracker)
{
    ServerContext ctx = {0};
    ctx.change_tracker = NULL;
    HTTPRequest req = {0};

    handle_undo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_undo_nothing)
{
    ChangeTracker ct = {0};
    stub_ct_undo_result = -1;

    ServerContext ctx = {0};
    ctx.change_tracker = &ct;
    HTTPRequest req = {0};

    handle_undo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"undo\":false"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_undo_success)
{
    int dummy_ct = 0;
    stub_ct_undo_result = 1024;

    ServerContext ctx = {0};
    ctx.change_tracker = (void *)&dummy_ct;
    HTTPRequest req = {0};

    handle_undo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"undo\":true"));
    ck_assert(strstr(captured_body, "\"bytes_restored\":1024"));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_redo
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_redo_no_tracker)
{
    ServerContext ctx = {0};
    ctx.change_tracker = NULL;
    HTTPRequest req = {0};

    handle_redo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_redo_nothing)
{
    int dummy_ct = 0;
    stub_ct_redo_result = -1;

    ServerContext ctx = {0};
    ctx.change_tracker = (void *)&dummy_ct;
    HTTPRequest req = {0};

    handle_redo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"redo\":false"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_redo_success)
{
    int dummy_ct = 0;
    stub_ct_redo_result = 2048;

    ServerContext ctx = {0};
    ctx.change_tracker = (void *)&dummy_ct;
    HTTPRequest req = {0};

    handle_redo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"redo\":true"));
    ck_assert(strstr(captured_body, "\"bytes_written\":2048"));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_health_detailed
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_health_detailed_no_sm)
{
    ServerContext ctx = {0};
    ctx.sm = NULL;
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};

    handle_health_detailed(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"session_enabled\":false"));
    ck_assert(strstr(captured_body, "\"state\":\"locked\""));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_health_detailed_with_sm)
{
    int dummy_sm = 0;
    ServerContext ctx = {0};
    ctx.sm = (void *)&dummy_sm;
    ctx.state = STATE_UNLOCKED;
    HTTPRequest req = {0};

    handle_health_detailed(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"session_enabled\":true"));
    ck_assert(strstr(captured_body, "\"state\":\"unlocked\""));

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_models
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_models_curl_unavailable)
{
    stub_curl_init_nonnull = 0;
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_perform_fails)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_URL_MALFORMAT;
    stub_models_json = NULL;
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_success)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[{\"name\":\"llama3:latest\"},{\"name\":\"mistral:7b\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "llama3:latest"));
    ck_assert(strstr(captured_body, "mistral:7b"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_empty_models_array)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_ollama_default_url)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[{\"name\":\"llama3:latest\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:11434/api/tags"));
    ck_assert(strstr(captured_body, "llama3:latest"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_ollama_explicit_query)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[{\"name\":\"llama3:latest\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=ollama&foo=1");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:11434/api/tags"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_signed_out_returns_empty_local_list)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"qwen2.5\"},{\"id\":\"deepseek-v3\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_OUT;
    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_curl_url, "");
    ck_assert_str_eq(captured_body, "{\"models\":[]}");

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_signed_in_uses_remote_catalog)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"gpt-4o-mini\"}]}";

    char tmpdir[] = "/tmp/test_models_token_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[providers]\nopenai = sk-model-test\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = 0;
    stub_openai_models[0] = "gpt-5.4";
    stub_openai_models[1] = "gpt-5.3-codex";
    stub_openai_models[2] = "gpt-5.2-codex";
    stub_openai_models_count = 3U;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_auth_header, "");
    ck_assert_str_eq(captured_curl_url, "");
    ck_assert(strstr(captured_body, "gpt-5.4"));
    ck_assert(strstr(captured_body, "gpt-5.3-codex"));
    ck_assert(strstr(captured_body, "gpt-5.2-codex"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_discovery_failure_uses_fallback)
{
    ServerContext ctx = {0};
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = -1;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 200);
    ck_assert_ptr_nonnull(strstr(captured_body, "gpt-5.5"));
    ck_assert_ptr_nonnull(strstr(captured_body, "gpt-5.3-codex-spark"));
    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_denied_returns_empty_list)
{
    /* 4xx entitlement denial must not surface the fallback catalog. */
    ServerContext ctx = {0};
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = OPENAI_MODELS_DENIED;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_body, "{\"models\":[]}");
    ck_assert_ptr_null(strstr(captured_body, "gpt-5.5"));
    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_zero_visible_returns_empty_list)
{
    /* A parsed-but-empty catalog must not fall back to the fixed list. */
    ServerContext ctx = {0};
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = OPENAI_MODELS_OK;
    stub_openai_models_count = 0U;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_body, "{\"models\":[]}");
    ck_assert_ptr_null(strstr(captured_body, "gpt-5.5"));
    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_lm_studio_alias_uses_openai)
{
    /* lm_studio is an alias for the static-token compatible endpoint. */
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"qwen2.5\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=lm_studio");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:1234/v1/models"));
    ck_assert(strstr(captured_body, "qwen2.5"));

    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_ignores_custom_public_base_url)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"local-model\"}]}";

    char tmpdir[] = "/tmp/test_models_conf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[openai]\nbase_url = http://localhost:1234\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = 0;
    stub_openai_models[0] = "gpt-5.4";
    stub_openai_models_count = 1U;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_curl_url, "");
    ck_assert(strstr(captured_body, "gpt-5.4"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_openai_compatible_uses_own_base_url)
{
    /* openai_compatible resolves its own base_url key, not openai's. */
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"local-model\"}]}";

    char tmpdir[] = "/tmp/test_models_conf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[openai_compatible]\nbase_url = http://localhost:4321\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai_compatible");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:4321/v1/models"));
    ck_assert(strstr(captured_body, "local-model"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}
END_TEST

START_TEST(test_handle_models_unknown_provider_empty_no_curl)
{
    stub_curl_init_nonnull = 1;
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=anthropic");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));
    ck_assert_str_eq(captured_curl_url, "");

    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * Suite
 * --------------------------------------------------------------------------- */

static void setup(void) { reset_stubs(); }
static void teardown(void) { reset_stubs(); }

Suite *routes_general_suite(void)
{
    Suite *s = suite_create("routes_general");

    TCase *tc = tcase_create("handle_health");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_health_ok);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_status");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_status_locked);
    tcase_add_test(tc, test_handle_status_setup);
    tcase_add_test(tc, test_handle_status_unlocked_valid_token);
    tcase_add_test(tc, test_handle_status_unlocked_bad_token);
    tcase_add_test(tc, test_handle_status_noop_token_unlocked_without_header);
    tcase_add_test(tc, test_handle_status_session_enabled);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_config");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_config_no_agent);
    tcase_add_test(tc, test_handle_config_with_agent);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_metrics");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_metrics_no_metrics);
    tcase_add_test(tc, test_handle_metrics_success);
    tcase_add_test(tc, test_handle_metrics_render_null);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_undo");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_undo_no_tracker);
    tcase_add_test(tc, test_handle_undo_nothing);
    tcase_add_test(tc, test_handle_undo_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_redo");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_redo_no_tracker);
    tcase_add_test(tc, test_handle_redo_nothing);
    tcase_add_test(tc, test_handle_redo_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_health_detailed");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_health_detailed_no_sm);
    tcase_add_test(tc, test_handle_health_detailed_with_sm);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_models");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_models_curl_unavailable);
    tcase_add_test(tc, test_handle_models_perform_fails);
    tcase_add_test(tc, test_handle_models_success);
    tcase_add_test(tc, test_handle_models_empty_models_array);
    tcase_add_test(tc, test_handle_models_ollama_default_url);
    tcase_add_test(tc, test_handle_models_ollama_explicit_query);
    tcase_add_test(tc, test_handle_models_openai_signed_out_returns_empty_local_list);
    tcase_add_test(tc, test_handle_models_openai_signed_in_uses_remote_catalog);
    tcase_add_test(tc,
                   test_handle_models_openai_discovery_failure_uses_fallback);
    tcase_add_test(tc, test_handle_models_openai_denied_returns_empty_list);
    tcase_add_test(tc, test_handle_models_openai_zero_visible_returns_empty_list);
    tcase_add_test(tc, test_handle_models_lm_studio_alias_uses_openai);
    tcase_add_test(tc, test_handle_models_openai_ignores_custom_public_base_url);
    tcase_add_test(tc, test_handle_models_openai_compatible_uses_own_base_url);
    tcase_add_test(tc, test_handle_models_unknown_provider_empty_no_curl);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_providers");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_providers_lists_available_providers);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_general_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
