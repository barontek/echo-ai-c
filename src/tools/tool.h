#ifndef ECHO_TOOL_H
#define ECHO_TOOL_H

#include <cjson/cJSON.h>

typedef struct {
    char *content;
    char *error;
    char *error_category;
} ToolResult;

typedef struct Tool Tool;

struct Tool {
    char *name;
    char *description;
    char *parameters_schema;
    ToolResult *(*execute)(Tool *self, const char *args_json);
    void (*destroy)(Tool *self);
    void *ctx;
};

ToolResult *tool_result_create(const char *content);
ToolResult *tool_result_error(const char *error, const char *category);
void tool_result_free(ToolResult *result);

#endif
