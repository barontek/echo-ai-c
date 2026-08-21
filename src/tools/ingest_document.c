/*
 * ingest_document.c - document ingestion tool: adds a file's or raw
 * content's text to the semantic search index for later retrieval.
 * Depends on: tool.h, semantic_search, safety, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tool.h"
#include "semantic_search.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} IngestCtx;

static ToolResult *ingest_document_execute(Tool *self, const char *args_json)
{
    IngestCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    cJSON *content_json = cJSON_GetObjectItem(args, "content");

    if ((!path_json || !cJSON_IsString(path_json)) &&
        (!content_json || !cJSON_IsString(content_json)))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'path' or 'content' argument", "validation_error");
    }

    const char *content = NULL;
    char *freed_content = NULL;

    if (content_json && cJSON_IsString(content_json))
    {
        freed_content = str_dup(cJSON_GetStringValue(content_json));
        content = freed_content;
    }
    else if (path_json && cJSON_IsString(path_json))
    {
        const char *path = cJSON_GetStringValue(path_json);
        if (!safety_check_path(ctx->safety, path))
        {
            cJSON_Delete(args);
            return tool_result_error("path rejected by safety check", "policy_denied");
        }
        char *resolved = safety_resolve_path_alloc(ctx->safety, path);
        if (!resolved)
        {
            cJSON_Delete(args);
            return tool_result_error("path resolution failed", "policy_denied");
        }

        struct stat st;
        if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode))
        {
            free(resolved);
            cJSON_Delete(args);
            return tool_result_error("file not found", "file_not_found");
        }

        FILE *f = fopen(resolved, "rb");
        free(resolved);
        if (!f) {
            cJSON_Delete(args);
            return tool_result_error("cannot open file", "execution_error");
        }

        freed_content = malloc((size_t)st.st_size + 1);
        if (!freed_content) {
            fclose(f); // NOLINT(cert-err33-c)
            cJSON_Delete(args);
            return tool_result_error("oom", "execution_error");
        }

        size_t read = fread(freed_content, 1, (size_t)st.st_size, f);
        fclose(f); // NOLINT(cert-err33-c)
        freed_content[read] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
        content = freed_content;
    }

    if (!content || !content[0])
    {
        free(freed_content);
        cJSON_Delete(args);
        return tool_result_error("empty content", "validation_error");
    }

    cJSON_Delete(args);

    int indexed = semantic_search_index_document(content);

    free(freed_content);
    if (indexed != 0)
        return tool_result_error("failed to index document (index full or OOM)",
                                 "execution_error");
    return tool_result_create("Document ingested and indexed for semantic search.");
}

static void ingest_document_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_ingest_document_create - construct the ingest_document tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_ingest_document_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    IngestCtx *ctx = calloc(1, sizeof(IngestCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("ingest_document");
    t->description = str_dup("Add a document to the semantic search index");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"File path to ingest\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"Raw text content to ingest (use instead of path)\"}"
        "},\"oneOf\":[{\"required\":[\"path\"]},{\"required\":[\"content\"]}]}"
    );
    t->execute = ingest_document_execute;
    t->destroy = ingest_document_destroy;
    t->ctx = ctx;
    return t;
}
