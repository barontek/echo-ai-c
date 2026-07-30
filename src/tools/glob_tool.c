#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"

typedef struct {
    SafetyConfig *safety;
} GlobCtx;

static ToolResult *glob_execute(Tool *self, const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *pattern_json = cJSON_GetObjectItem(args, "pattern");
    if (!pattern_json || !cJSON_IsString(pattern_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'pattern' argument", "validation_error");
    }

    char *pattern = str_dup(cJSON_GetStringValue(pattern_json));
    cJSON_Delete(args);

    GlobCtx *gctx = (GlobCtx *)self->ctx;
    if (gctx && gctx->safety && !safety_check_path(gctx->safety, pattern))
    {
        free(pattern);
        return tool_result_error("pattern rejected by safety check", "validation_error");
    }

    char *workspace_pattern = NULL;
    if (asprintf(&workspace_pattern, "%s/%s", gctx->safety->workspace, pattern) < 0)
        workspace_pattern = NULL;
    free(pattern);
    if (!workspace_pattern)
        return tool_result_error("pattern resolution failed", "execution_error");

    glob_t globbuf;
    int ret = glob(workspace_pattern, GLOB_NOSORT, NULL, &globbuf);
    free(workspace_pattern);

    if (ret != 0 && ret != GLOB_NOMATCH)
    {
        return tool_result_error("glob pattern error", "execution_error");
    }

    char buffer[16384] = {0};
    size_t pos = 0;

    for (size_t i = 0; i < globbuf.gl_pathc; i++)
    {
        if (!safety_path_is_within_workspace(gctx->safety, globbuf.gl_pathv[i]))
            continue;
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%s\n", globbuf.gl_pathv[i]);
        if (pos >= sizeof(buffer) - 1) break;
    }

    globfree(&globbuf);

    ToolResult *tr = tool_result_create(buffer[0] ? buffer : "(no matches)");
    return tr;
}

static void glob_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

Tool *tool_glob_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    GlobCtx *ctx = calloc(1, sizeof(GlobCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("glob");
    t->description = str_dup("Match files using a glob pattern");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern (e.g. **\\/*.c)\"}"
        "},\"required\":[\"pattern\"]}"
    );
    t->execute = glob_execute;
    t->destroy = glob_destroy;
    t->ctx = ctx;
    return t;
}
