#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "../utils/string_utils.h"

ToolResult *tool_result_create(const char *content)
{
    ToolResult *r = calloc(1, sizeof(ToolResult));
    if (!r) return NULL;
    r->content = str_dup(content ? content : "");
    return r;
}

ToolResult *tool_result_error(const char *error, const char *category)
{
    ToolResult *r = calloc(1, sizeof(ToolResult));
    if (!r) return NULL;
    r->error = str_dup(error ? error : "unknown error");
    r->error_category = str_dup(category ? category : "execution_error");
    return r;
}

void tool_result_free(ToolResult *result)
{
    if (!result) return;
    free(result->content);
    free(result->error);
    free(result->error_category);
    free(result);
}
