/*
 * routes_chat.c - HTTP chat endpoints: the blocking POST /api/chat turn
 * runner and the EventSource GET /api/stream, which forwards agent
 * chunks as SSE frames. Depends on: cJSON, middleware, agent, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_chat.h"
#include "../middleware.h"
#include "../../agent/agent.h"
#include "../../utils/logging.h"

#ifdef ROUTES_CHAT_TEST
/* Fault-injection knobs: let a test force the Nth asprintf()/calloc() in
 * this file to fail, so the OOM paths in handle_sse_stream can be
 * exercised. Production builds never see these; only translation units
 * compiled with -DSESSION... -DROUTES_CHAT_TEST=1 do. The shim bodies are
 * defined before the #defines so they call the real functions. */
static int rc_alloc_counter = 0;
static int rc_alloc_fail_at = -1;

void routes_chat_test_set_alloc_fail(int nth_allocation)
{
    rc_alloc_counter = 0;
    rc_alloc_fail_at = nth_allocation;
}

static int rc_test_asprintf(char **strp, const char *fmt, ...)
{
    rc_alloc_counter++;
    if (rc_alloc_counter == rc_alloc_fail_at) { *strp = NULL; return -1; }
    va_list ap;
    va_start(ap, fmt);
    int rc = vasprintf(strp, fmt, ap);
    va_end(ap);
    return rc;
}

static void *rc_test_calloc(size_t nmemb, size_t size)
{
    rc_alloc_counter++;
    if (rc_alloc_counter == rc_alloc_fail_at) return NULL;
    return calloc(nmemb, size);
}

#define asprintf rc_test_asprintf
#define calloc rc_test_calloc
#endif

void handle_chat(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (ctx->metrics) metrics_counter_inc(ctx->metrics, "echo_chat_requests_total", "Total chat requests");

    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    if (!req->body || req->body_len == 0)
    {
        server_response_error(client, 400, "missing message");
        return;
    }

    cJSON *json = cJSON_Parse(req->body);
    if (!json)
    {
        server_response_error(client, 400, "invalid json");
        return;
    }

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    if (!msg || !msg->valuestring)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "missing message field");
        return;
    }

    if (!ctx->agent)
    {
        cJSON_Delete(json);
        server_response_error(client, 500, "agent not initialized");
        return;
    }

    LLMResponse *resp = agent_run(ctx->agent, msg->valuestring);
    cJSON_Delete(json);

    if (!resp)
    {
        server_response_error(client, 500, "agent returned no response");
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "content", resp->content ? resp->content : "");
    cJSON_AddBoolToObject(r, "has_tools", resp->tool_calls_count > 0);
    if (resp->thinking)
        cJSON_AddStringToObject(r, "thinking", resp->thinking);

    if (resp->tool_calls_count > 0)
    {
        cJSON *tc_arr = cJSON_CreateArray();
        for (int i = 0; i < resp->tool_calls_count; i++)
        {
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "name", resp->tool_calls[i].name ? resp->tool_calls[i].name : "");
            cJSON_AddStringToObject(tc, "arguments", resp->tool_calls[i].arguments ? resp->tool_calls[i].arguments : "{}");
            cJSON_AddItemToArray(tc_arr, tc);
        }
        cJSON_AddItemToObject(r, "tool_calls", tc_arr);
    }

    char *str = cJSON_PrintUnformatted(r);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(r);
    llm_response_free(resp);
}

typedef struct {
    Agent *agent;
    Client *client;
} SSECtx;

static void sse_on_chunk(const char *chunk, void *userdata)
{
    SSECtx *c = (SSECtx *)userdata;
    if (!c || !c->client) return;

    cJSON *event = cJSON_CreateObject();
    if (!event) return;
    cJSON_AddStringToObject(event, "type", "content");
    cJSON_AddStringToObject(event, "content", chunk ? chunk : "");
    char *json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (!json) return;

    char *sse = NULL;
    if (asprintf(&sse, "data: %s\n\n", json) < 0) sse = NULL;
    free(json);
    if (!sse) return;
    server_sse_write(c->client, sse);
    free(sse);
}

void handle_sse_stream(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    /* EventSource cannot set X-Unlock-Token header; the dispatch path
     * already checks the query-param token via the route-table flag.
     * This in-handler check catches header-based tokens as a fallback
     * for non-browser clients that can set the header but bypassed the
     * route table (if ever called directly without the dispatch check). */
    if (!middleware_check_unlock(req, ctx) &&
        !middleware_check_unlock_query(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    if (!ctx->agent) { server_response_error(client, 500, "no agent"); return; }

    char *headers = NULL;
    if (asprintf(&headers,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n") < 0)
    {
        /* Nothing has been written yet, so a normal error response is
         * still valid; write_done closes the client after the flush. */
        server_response_error(client, 500, "internal error");
        return;
    }

    server_sse_write(client, headers);
    free(headers);

    SSECtx *c = calloc(1, sizeof(SSECtx));
    if (!c)
    {
        /* The 200 SSE headers are already on the wire; a 500 would be
         * invalid now, so signal the failure inside the stream and close
         * instead of leaving the connection dangling. */
        server_sse_write(client,
                         "data: {\"type\":\"error\",\"message\":\"internal error\"}\n\n");
        client_close(client);
        return;
    }
    c->agent = ctx->agent;
    c->client = client;

    LLMResponse *resp = agent_run_streaming(ctx->agent, "", sse_on_chunk, c);
    free(c);

    if (resp)
    {
        server_sse_write(client, "data: {\"type\":\"done\"}\n\n");
        llm_response_free(resp);
    }
    else
    {
        server_sse_write(client, "data: {\"type\":\"error\",\"message\":\"no response\"}\n\n");
    }

    client_close(client);
}
