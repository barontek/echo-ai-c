/*
 * tool_memory.c - memory facts tool: get/set/delete/list persistent
 * user-memory facts through the session manager's SQLite-backed memory
 * module. Depends on: tool.h, registry, session/memory, string_utils,
 * logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TOOL_MEMORY_TEST
#include <stdarg.h>
#endif

#include <cjson/cJSON.h>

#include "tool.h"
#include "registry.h"
#include "../session/memory.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef TOOL_MEMORY_TEST
/* Test-only allocator fault injection: shared counter across str_dup,
 * asprintf and realloc so tests can fail the Nth allocation inside
 * tool_memory_create / memory_execute's list path. Only the test target
 * defines TOOL_MEMORY_TEST. */
static int tool_memory_test_alloc_counter = 0;
static int tool_memory_test_alloc_fail_at = -1;

void tool_memory_test_set_alloc_fail(int nth_allocation)
{
    tool_memory_test_alloc_counter = 0;
    tool_memory_test_alloc_fail_at = nth_allocation;
}

static char *tool_memory_test_strdup(const char *s)
{
    tool_memory_test_alloc_counter++;
    if (tool_memory_test_alloc_counter == tool_memory_test_alloc_fail_at)
        return NULL;
    return str_dup(s);
}

static int tool_memory_test_asprintf(char **strp, const char *fmt, ...)
{
    tool_memory_test_alloc_counter++;
    if (tool_memory_test_alloc_counter == tool_memory_test_alloc_fail_at)
    {
        *strp = NULL;
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int rc = vasprintf(strp, fmt, ap);
    va_end(ap);
    return rc;
}

static void *tool_memory_test_realloc(void *ptr, size_t size)
{
    tool_memory_test_alloc_counter++;
    if (tool_memory_test_alloc_counter == tool_memory_test_alloc_fail_at)
        return NULL;
    return realloc(ptr, size);
}

#define str_dup tool_memory_test_strdup
#define asprintf tool_memory_test_asprintf
#define realloc tool_memory_test_realloc
#endif

typedef struct {
    SafetyConfig *safety;
} MemoryToolCtx;

static ToolResult *memory_execute(Tool *self, const char *args_json)
{
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *action = cJSON_GetObjectItem(args, "action");
    if (!action || !cJSON_IsString(action))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'action' (get/set/delete/list)", "validation_error");
    }

    SessionManager *sm = registry_get_session_manager();
    if (!sm)
    {
        cJSON_Delete(args);
        return tool_result_error("session manager not available", "execution_error");
    }

    const char *act = cJSON_GetStringValue(action);

    if (strcmp(act, "get") == 0)
    {
        cJSON *key = cJSON_GetObjectItem(args, "key");
        if (!key || !cJSON_IsString(key))
        {
            cJSON_Delete(args);
            return tool_result_error("missing 'key' for get", "validation_error");
        }
        int mem_error = 0;
        char *val = memory_get_dup(sm->db, cJSON_GetStringValue(key), &mem_error);
        cJSON_Delete(args);
        if (mem_error)
            return tool_result_error("memory store error", "execution_error");
        ToolResult *tr = tool_result_create(val ? val : "(not found)");
        free(val);
        return tr;
    }

    if (strcmp(act, "set") == 0)
    {
        cJSON *key = cJSON_GetObjectItem(args, "key");
        cJSON *value = cJSON_GetObjectItem(args, "value");
        if (!key || !cJSON_IsString(key) || !value || !cJSON_IsString(value))
        {
            cJSON_Delete(args);
            return tool_result_error("missing 'key' or 'value' for set", "validation_error");
        }
        int rc = memory_set(sm->db, cJSON_GetStringValue(key), cJSON_GetStringValue(value));
        cJSON_Delete(args);
        if (rc != 0)
            return tool_result_error("failed to set memory", "execution_error");
        return tool_result_create("ok");
    }

    if (strcmp(act, "delete") == 0)
    {
        cJSON *key = cJSON_GetObjectItem(args, "key");
        if (!key || !cJSON_IsString(key))
        {
            cJSON_Delete(args);
            return tool_result_error("missing 'key' for delete", "validation_error");
        }
        int rc = memory_delete(sm->db, cJSON_GetStringValue(key));
        cJSON_Delete(args);
        if (rc != 0)
            return tool_result_error("failed to delete memory", "execution_error");
        return tool_result_create("deleted");
    }

    if (strcmp(act, "list") == 0)
    {
        cJSON_Delete(args);
        int count = 0;
        int mem_error = 0;
        MemoryFact *facts = memory_list_all(sm->db, &count, &mem_error);
        if (!facts && mem_error)
            return tool_result_error("memory store error", "execution_error");
        if (!facts) return tool_result_create("(no memory stored)");

        char *result = NULL;
        size_t len = 0;
        for (int i = 0; i < count; i++)
        {
            char *line = NULL;
            if (asprintf(&line, "%s: %s\n", facts[i].key, facts[i].value) < 0) continue;
            size_t llen = strlen(line);
            char *newr = realloc(result, len + llen + 1);
            if (!newr) {
                free(line);
                continue;
            }
            result = newr;
            memcpy(result + len, line, llen + 1);
            len += llen;
            free(line);
        }
        memory_facts_free(facts, count);
        if (!result) return tool_result_create("(no memory stored)");
        ToolResult *tr = tool_result_create(result);
        free(result);
        return tr;
    }

    cJSON_Delete(args);
    return tool_result_error("unknown action (use get/set/delete/list)", "validation_error");
}

static void memory_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_memory_create - construct the memory tool
 * @safety: borrowed SafetyConfig; retained in the tool's context but not
 * consulted by the execute path
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_memory_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    MemoryToolCtx *ctx = calloc(1, sizeof(MemoryToolCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("memory");
    t->description = str_dup("Store and retrieve user memory facts (get/set/delete/list)");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"action\":{\"type\":\"string\",\"enum\":[\"get\",\"set\",\"delete\",\"list\"],\"description\":\"Operation to perform\"},"
        "\"key\":{\"type\":\"string\",\"description\":\"Memory key (required for get/set/delete)\"},"
        "\"value\":{\"type\":\"string\",\"description\":\"Memory value (required for set)\"}"
        "},\"required\":[\"action\"]}"
    );
    if (!t->name || !t->description || !t->parameters_schema)
    {
        free(t->name);
        free(t->description);
        free(t->parameters_schema);
        free(ctx);
        free(t);
        return NULL;
    }
    t->execute = memory_execute;
    t->destroy = memory_destroy;
    t->ctx = ctx;
    return t;
}
