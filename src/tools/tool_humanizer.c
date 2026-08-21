/*
 * tool_humanizer.c - content formatting tool: restyles text as a
 * paragraph, bulleted list, or 500-character summary. Depends on:
 * tool.h, registry, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "tool.h"
#include "registry.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} HumanizerCtx;

static ToolResult *humanizer_execute(Tool *self, const char *args_json)
{
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *content = cJSON_GetObjectItem(args, "content");
    if (!content || !cJSON_IsString(content))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'content' argument", "validation_error");
    }

    char *text = str_dup(cJSON_GetStringValue(content));

    char *style = str_dup("paragraph");
    cJSON *style_item = cJSON_GetObjectItem(args, "style");
    if (style_item && cJSON_IsString(style_item))
    {
        free(style);
        style = str_dup(cJSON_GetStringValue(style_item));
    }

    cJSON_Delete(args);

    char *result = NULL;

    if (strcmp(style, "bullet") == 0)
    {
        cJSON *lines = cJSON_Parse(text);
        if (cJSON_IsArray(lines))
        {
            int count = cJSON_GetArraySize(lines);
            size_t total = 0;
            for (int i = 0; i < count; i++)
            {
                cJSON *item = cJSON_GetArrayItem(lines, i);
                const char *val = cJSON_IsString(item) ? cJSON_GetStringValue(item) : "";
                total += strlen(val) + 4;
            }
            result = malloc(total + 1);
            if (result)
            {
                result[0] = '\0';
                for (int i = 0; i < count; i++)
                {
                    cJSON *item = cJSON_GetArrayItem(lines, i);
                    const char *val = cJSON_IsString(item) ? cJSON_GetStringValue(item) : "";
                    size_t cur = strlen(result);
                    snprintf(result + cur, total - cur + 1, "- %s\n", val); // NOLINT(cert-err33-c)
                }
            }
            cJSON_Delete(lines);
        }
        else
        {
            cJSON_Delete(lines);
            result = str_dup(text);
        }
    }
    else if (strcmp(style, "summary") == 0)
    {
        int len = strlen(text);
        int slen = len < 500 ? len : 500;
        result = malloc(slen + 1);
        if (result)
        {
            memcpy(result, text, slen);
            result[slen] = '\0';
            if (slen < len)
            {
                char *tmp = NULL;
                if (asprintf(&tmp, "%s...", result) >= 0)
                {
                    free(result);
                    result = tmp;
                }
            }
        }
    }
    else
    {
        result = str_dup(text);
    }

    if (!result)
    {
        free(text);
        free(style);
        return tool_result_error("oom", "execution_error");
    }

    ToolResult *tr = tool_result_create(result);
    free(result);
    free(text);
    free(style);
    return tr;
}

static void humanizer_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_humanizer_create - construct the humanizer tool
 * @safety: borrowed SafetyConfig; retained in the tool's context but not
 * consulted by the execute path
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_humanizer_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    HumanizerCtx *ctx = calloc(1, sizeof(HumanizerCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("humanizer");
    t->description = str_dup("Format content for readability (paragraph, bullet, summary styles)");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"content\":{\"type\":\"string\",\"description\":\"Content to format\"},"
        "\"style\":{\"type\":\"string\",\"enum\":[\"paragraph\",\"bullet\",\"summary\"],\"description\":\"Output style\"}"
        "},\"required\":[\"content\"]}"
    );
    t->execute = humanizer_execute;
    t->destroy = humanizer_destroy;
    t->ctx = ctx;
    return t;
}
