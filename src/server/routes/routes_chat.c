#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_chat.h"
#include "../middleware.h"
#include "../../agent/agent.h"
#include "../../utils/logging.h"

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
    char *sse = NULL;
    if (asprintf(&sse, "data: {\"type\":\"content\",\"content\":%s}\n\n",
                 chunk ? chunk : "") < 0) return;
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
        "\r\n") < 0) return;

    server_sse_write(client, headers);
    free(headers);

    SSECtx *c = calloc(1, sizeof(SSECtx));
    if (!c) return;
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

