/*
 * registry.c - Owns the process-wide tool table and the shared singletons
 * (search provider, session manager, OAuth, delegate config, ask-user
 * callback). Depends on: every built-in tool module, change_tracker,
 * string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"
#include "tool.h"
#include "../change_tracker/change_tracker.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef REGISTRY_TEST
static int reg_alloc_counter = 0;
static int reg_alloc_fail_at = -1;

void registry_test_set_alloc_fail(int nth_allocation)
{
    reg_alloc_counter = 0;
    reg_alloc_fail_at = nth_allocation;
}

/* B1: free tally so tests can assert registry_destroy released the
 * delegate-config strings (defined before the #define so its own body
 * calls the real free). */
static int reg_free_count = 0;
static void reg_test_free(void *p)
{
    reg_free_count++;
    free(p);
}
void registry_test_free_reset(void) { reg_free_count = 0; }
int registry_test_free_tally(void) { return reg_free_count; }

static char *reg_test_strdup(const char *s)
{
    reg_alloc_counter++;
    if (reg_alloc_counter == reg_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup reg_test_strdup
#define free reg_test_free
#endif

#define MAX_TOOLS 32

static Tool *tools[MAX_TOOLS];
static int tool_count = 0;

#ifndef REGISTRY_TEST
static SafetyConfig *safety_global = NULL;
#endif
static SearchProvider *search_provider_global = NULL;
static SessionManager *session_manager_global = NULL;
static OpenAIOAuth *openai_oauth_global = NULL;

typedef struct {
    char *provider_name;
    char *base_url;
    char *api_token;
    char *model;
    int num_ctx;
    int keep_alive_secs;
    double temperature;
    int timeout;
    int max_iterations;
} DelegateConfig;

static DelegateConfig delegate_config = {0};

static char *(*ask_user_cb)(const char *, void *) = NULL;
static void *ask_user_cb_data = NULL;

static Tool *registry_find_registered(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < tool_count; i++)
    {
        if (strcmp(tools[i]->name, name) == 0)
            return tools[i];
    }
    return NULL;
}

Tool *tool_bash_create(SafetyConfig *safety);
Tool *tool_read_file_create(SafetyConfig *safety);
Tool *tool_write_file_create(SafetyConfig *safety);
Tool *tool_list_dir_create(SafetyConfig *safety);
Tool *tool_glob_create(SafetyConfig *safety);
Tool *tool_grep_create(SafetyConfig *safety);
Tool *tool_web_fetch_create(SafetyConfig *safety);
Tool *tool_web_search_create(SafetyConfig *safety);
Tool *tool_edit_create(SafetyConfig *safety);
Tool *tool_python_execute_create(SafetyConfig *safety);
Tool *tool_rest_api_create(SafetyConfig *safety);
Tool *tool_notes_create(SafetyConfig *safety);
Tool *tool_git_create(SafetyConfig *safety);
Tool *tool_ingest_document_create(SafetyConfig *safety);
Tool *tool_semantic_search_create(SafetyConfig *safety);
Tool *tool_deep_search_create(SafetyConfig *safety);
Tool *tool_memory_create(SafetyConfig *safety);
Tool *tool_delegate_create(SafetyConfig *safety);
Tool *tool_sqlite_query_create(SafetyConfig *safety);
Tool *tool_sqlite_schema_create(SafetyConfig *safety);
Tool *tool_ask_user_create(SafetyConfig *safety);
Tool *tool_humanizer_create(SafetyConfig *safety);
Tool *tool_browser_create(SafetyConfig *safety);

/* Test builds wire the registry by hand, so the real factories are skipped
 * to keep the test binary from linking every built-in tool module. */
#ifndef REGISTRY_TEST
int registry_init(SafetyConfig *safety)
{
    safety_global = safety;
    tool_count = 0;

    /* A failed registration (OOM in a tool factory, full table) leaves
     * the tool absent; report the count so the caller can log context
     * instead of a silently partial tool table. */
    int failed = 0;
    failed += registry_register(tool_bash_create(safety));
    failed += registry_register(tool_read_file_create(safety));
    failed += registry_register(tool_write_file_create(safety));
    failed += registry_register(tool_list_dir_create(safety));
    failed += registry_register(tool_glob_create(safety));
    failed += registry_register(tool_grep_create(safety));
    failed += registry_register(tool_web_fetch_create(safety));
    failed += registry_register(tool_web_search_create(safety));
    failed += registry_register(tool_edit_create(safety));
    failed += registry_register(tool_python_execute_create(safety));
    failed += registry_register(tool_rest_api_create(safety));
    failed += registry_register(tool_notes_create(safety));
    failed += registry_register(tool_git_create(safety));
    failed += registry_register(tool_ingest_document_create(safety));
    failed += registry_register(tool_semantic_search_create(safety));
    failed += registry_register(tool_deep_search_create(safety));
    failed += registry_register(tool_memory_create(safety));
    failed += registry_register(tool_delegate_create(safety));
    failed += registry_register(tool_sqlite_query_create(safety));
    failed += registry_register(tool_sqlite_schema_create(safety));
    failed += registry_register(tool_ask_user_create(safety));
    failed += registry_register(tool_humanizer_create(safety));
    failed += registry_register(tool_browser_create(safety));
    return failed;
}
#endif

int registry_register(Tool *tool)
{
    if (tool_count >= MAX_TOOLS || !tool) return -1;

    tool->enabled = 0; /* disabled by default, enabled via config */
    tools[tool_count++] = tool;
    log_info("registered tool", "name", tool->name, NULL);
    return 0;
}

int registry_set_enabled(const char *names)
{
    if (!names || !names[0]) return 0;

    char *buf = str_dup(names);
    if (!buf) return -1;

    char *save = NULL;
    char *tok = strtok_r(buf, ", ", &save);
    while (tok)
    {
        while (*tok == ' ') tok++;
        Tool *t = registry_find_registered(tok);
        if (t)
        {
            t->enabled = 1;
            log_info("tool enabled", "name", tok, NULL);
        }
        tok = strtok_r(NULL, ", ", &save);
    }
    free(buf);
    return 0;
}

Tool *registry_get(const char *name)
{
    Tool *tool = registry_find_registered(name);
    return tool && tool->enabled ? tool : NULL;
}

char *registry_schemas_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < tool_count; i++)
    {
        if (!tools[i]->enabled) continue;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", tools[i]->name);
        cJSON_AddStringToObject(fn, "description", tools[i]->description ? tools[i]->description : "");

        cJSON *params = cJSON_Parse(tools[i]->parameters_schema);
        if (params)
            cJSON_AddItemToObject(fn, "parameters", params);
        else
            cJSON_AddStringToObject(fn, "parameters", tools[i]->parameters_schema ? tools[i]->parameters_schema : "{}");

        cJSON_AddItemToObject(t, "function", fn);
        cJSON_AddItemToArray(arr, t);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int registry_count(void)
{
    return tool_count;
}

int registry_enabled_count(void)
{
    int n = 0;
    for (int i = 0; i < tool_count; i++)
    {
        if (tools[i]->enabled) n++;
    }
    return n;
}

#ifndef REGISTRY_TEST
/* Same test-build reason as registry_init: no write_file tool to wire up. */
void registry_set_change_tracker(ChangeTracker *ct)
{
    void tool_write_file_set_change_tracker(Tool *tool, ChangeTracker *ct);
    for (int i = 0; i < tool_count; i++)
    {
        if (strcmp(tools[i]->name, "write_file") == 0)
            tool_write_file_set_change_tracker(tools[i], ct);
    }
}

void registry_set_browser_headless(int headless)
{
    void tool_browser_set_headless(Tool *tool, int headless);
    for (int i = 0; i < tool_count; i++)
    {
        if (strcmp(tools[i]->name, "browser") == 0)
            tool_browser_set_headless(tools[i], headless);
    }
}
#endif

void registry_set_search_provider(SearchProvider *sp)
{
    search_provider_global = sp;
}

SearchProvider *registry_get_search_provider(void)
{
    return search_provider_global;
}

void registry_set_session_manager(SessionManager *sm)
{
    session_manager_global = sm;
}

SessionManager *registry_get_session_manager(void)
{
    return session_manager_global;
}

void registry_set_openai_oauth(OpenAIOAuth *auth)
{
    openai_oauth_global = auth;
}

OpenAIOAuth *registry_get_openai_oauth(void)
{
    return openai_oauth_global;
}

void registry_set_delegate_config(const char *provider_name, const char *base_url,
                                   const char *api_token, const char *model,
                                   int num_ctx, int keep_alive_secs,
                                   double temperature, int timeout, int max_iterations)
{
    char *pn = provider_name ? str_dup(provider_name) : NULL;
    char *bu = base_url ? str_dup(base_url) : NULL;
    char *tk = api_token ? str_dup(api_token) : NULL;
    char *md = model ? str_dup(model) : NULL;
    if ((provider_name && !pn) || (base_url && !bu) ||
        (api_token && !tk) || (model && !md))
    {
        free(pn); free(bu); free(tk); free(md); return;
    }
    /* Commit only after every copy succeeded, so a failed set leaves the
     * previously installed config intact instead of half-replaced. */
    free(delegate_config.provider_name);
    free(delegate_config.base_url);
    free(delegate_config.api_token);
    free(delegate_config.model);
    delegate_config.provider_name = pn;
    delegate_config.base_url = bu;
    delegate_config.api_token = tk;
    delegate_config.model = md;
    delegate_config.num_ctx = num_ctx;
    delegate_config.keep_alive_secs = keep_alive_secs;
    delegate_config.temperature = temperature;
    delegate_config.timeout = timeout;
    delegate_config.max_iterations = max_iterations;
}

int registry_get_delegate_config(const char **provider_name, const char **base_url,
                                  const char **api_token, const char **model,
                                  int *num_ctx, int *keep_alive_secs,
                                  double *temperature, int *timeout, int *max_iterations)
{
    if (!delegate_config.provider_name) return -1;
    if (provider_name) *provider_name = delegate_config.provider_name;
    if (base_url) *base_url = delegate_config.base_url;
    if (api_token) *api_token = delegate_config.api_token;
    if (model) *model = delegate_config.model;
    if (num_ctx) *num_ctx = delegate_config.num_ctx;
    if (keep_alive_secs) *keep_alive_secs = delegate_config.keep_alive_secs;
    if (temperature) *temperature = delegate_config.temperature;
    if (timeout) *timeout = delegate_config.timeout;
    if (max_iterations) *max_iterations = delegate_config.max_iterations;
    return 0;
}

void registry_set_ask_user_callback(char *(*cb)(const char *, void *), void *userdata)
{
    ask_user_cb = cb;
    ask_user_cb_data = userdata;
}

char *registry_invoke_ask_user(const char *question)
{
    if (ask_user_cb) return ask_user_cb(question, ask_user_cb_data);
    return NULL;
}

int registry_has_ask_user_callback(void)
{
    return ask_user_cb != NULL;
}

void registry_destroy(void)
{
    /* Only registry-owned state is released: tools (via destroy), the
     * search provider (destroyed), the OAuth pointer (cleared), and the
     * delegate config strings (owned since registry_set_delegate_config
     * str_dups them). The borrowed session-manager reference and ask-user
     * callback are left in place. */
    openai_oauth_global = NULL;
    for (int i = 0; i < tool_count; i++)
    {
        if (tools[i]->destroy) tools[i]->destroy(tools[i]);
    }
    tool_count = 0;
    if (search_provider_global)
    {
        search_provider_global->destroy(search_provider_global);
        search_provider_global = NULL;
    }
    free(delegate_config.provider_name);
    free(delegate_config.base_url);
    free(delegate_config.api_token);
    free(delegate_config.model);
    memset(&delegate_config, 0, sizeof(delegate_config));
}
