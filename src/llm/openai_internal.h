/*
 * openai_internal.h - shared constants and state for the Codex provider
 * split across openai_request/stream/response units. Documented
 * exception to the one-header-per-module rule: the streaming parser and
 * credential state cross unit boundaries, so they live here instead of
 * being duplicated. Not installed or included outside src/llm.
 * Depends on: openai.h, message.h, http_client.h.
 */

#ifndef ECHO_OPENAI_INTERNAL_H
#define ECHO_OPENAI_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "openai.h"
#include "../agent/message.h"
#include "../utils/http_client.h"

#define CODEX_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"
#define CODEX_MODELS_ENDPOINT "https://chatgpt.com/backend-api/codex/models"
#define OPENAI_MAX_REQUEST_BYTES ((size_t)16U * 1024U * 1024U)
#define OPENAI_MAX_RESPONSE_BYTES ((size_t)32U * 1024U * 1024U)
#define OPENAI_MAX_SSE_EVENT_BYTES ((size_t)4U * 1024U * 1024U)
#define OPENAI_MAX_MODELS_RESPONSE_BYTES ((size_t)4U * 1024U * 1024U)
#define OPENAI_MAX_MODELS 512U
#define OPENAI_MAX_MODEL_NAME_BYTES 256U
#define OPENAI_MAX_TOKEN_BYTES 16384U
#define OPENAI_MAX_ACCOUNT_BYTES 2048U
#define OPENAI_ERROR_FIELD_BYTES 96U

#ifndef ECHO_AI_VERSION
#define ECHO_AI_VERSION "0.1.0"
#endif

/* The Codex backend filters its catalog by minimal_client_version, so this
 * must track a current official Codex CLI release, not Echo's own version. */
#define CODEX_CLIENT_VERSION "0.146.0"

/* Access token + optional account id used to build request headers. */
typedef struct {
    char *token;
    char *account;
} Credentials;

/* Streaming emits one function call across several events keyed
 * differently (output_index, item id, or call id); the map binds all
 * three keys to one ToolCall slot so deltas can append in place. */
typedef struct {
    int output_index;
    int response_index;
    char *item_id;
} FunctionCallMap;

typedef struct {
    LLMResponse *response;
    FunctionCallMap *calls;
    size_t calls_count;
    char *line;
    size_t line_len;
    size_t line_cap;
    HttpBuffer event_data;
    void (*on_chunk)(const char *, void *);
    void *userdata;
    size_t received_bytes;
    int terminal_seen;
    int completed;
    int failed;
    int thinking_open;       /* 1 while a <think> block is open in content */
    int summary_deltas_seen; /* 1 after the first reasoning-summary delta */
} StreamParser;

#endif /* ECHO_OPENAI_INTERNAL_H */
