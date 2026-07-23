#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"
#include "tool.h"
#include "../change_tracker/change_tracker.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#define MAX_TOOLS 32

static Tool *tools[MAX_TOOLS];
static int tool_count = 0;

static SafetyConfig *safety_global = NULL;

Tool *tool_bash_create(SafetyConfig *safety);
Tool *tool_read_file_create(SafetyConfig *safety);
Tool *tool_write_file_create(SafetyConfig *safety);
Tool *tool_list_dir_create(SafetyConfig *safety);
Tool *tool_glob_create(SafetyConfig *safety);
Tool *tool_grep_create(SafetyConfig *safety);
Tool *tool_web_fetch_create(SafetyConfig *safety);
Tool *tool_web_search_create(SafetyConfig *safety);

void registry_init(SafetyConfig *safety)
{
    safety_global = safety;
    tool_count = 0;

    registry_register(tool_bash_create(safety));
    registry_register(tool_read_file_create(safety));
    registry_register(tool_write_file_create(safety));
    registry_register(tool_list_dir_create(safety));
    registry_register(tool_glob_create(safety));
    registry_register(tool_grep_create(safety));
    registry_register(tool_web_fetch_create(safety));
    registry_register(tool_web_search_create(safety));
}

void registry_register(Tool *tool)
{
    if (tool_count >= MAX_TOOLS || !tool) return;

    tools[tool_count++] = tool;
    log_info("registered tool", "name", tool->name, NULL);
}

Tool *registry_get(const char *name)
{
    for (int i = 0; i < tool_count; i++)
    {
        if (strcmp(tools[i]->name, name) == 0)
            return tools[i];
    }
    return NULL;
}

char *registry_schemas_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < tool_count; i++)
    {
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

void registry_set_change_tracker(ChangeTracker *ct)
{
    void tool_write_file_set_change_tracker(Tool *tool, ChangeTracker *ct);
    for (int i = 0; i < tool_count; i++)
    {
        if (strcmp(tools[i]->name, "write_file") == 0)
            tool_write_file_set_change_tracker(tools[i], ct);
    }
}

void registry_destroy(void)
{
    for (int i = 0; i < tool_count; i++)
    {
        if (tools[i]->destroy) tools[i]->destroy(tools[i]);
    }
    tool_count = 0;
}
