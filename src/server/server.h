#ifndef ECHO_SERVER_H
#define ECHO_SERVER_H

#include <uv.h>
#include "../config/config.h"
#include "../agent/agent.h"
#include "../session/session_manager.h"
#include "../tools/registry.h"
#include "../safety/safety.h"
#include "../utils/rate_limiter.h"
#include "../utils/metrics.h"
#include "../change_tracker/change_tracker.h"

typedef enum {
    STATE_LOCKED,
    STATE_SETUP,
    STATE_UNLOCKED
} ServerState;

typedef struct {
    ServerState state;
    Agent *agent;
    /* D2: per-connection Agent instances for the WS path are minted from
     * this config in routes_ws_chat_init and destroyed in ws_chat_on_close.
     * The shared `agent` field remains used by the REST /chat path only
     * (single-threaded per libuv request). Stored by value so its inner
     * string pointers alias Conf strings (which outlive the server). */
    AgentConfig agent_cfg;
    SessionManager *sm;
    SafetyConfig *safety;
    char *config_path;
    Conf *conf;
    char *unlock_token;
    unsigned long auth_generation;
    uv_loop_t *loop;
    int port;
    RateLimiter *rate_limiter;
    Metrics *metrics;
    ChangeTracker *change_tracker;
} ServerContext;

typedef struct Client Client;

typedef struct {
    char method[16];
    char path[1024];
    char query[512];
    char headers[4096];
    char *body;
    size_t body_len;
    struct sockaddr_storage addr;
    char ip[64];
    Client *client;
    char req_id[32];
} HTTPRequest;

typedef void (*route_handler)(HTTPRequest *req, Client *client, ServerContext *ctx);

int server_start(ServerContext *ctx);
void server_stop(ServerContext *ctx);
void server_response(Client *client, int status, const char *content_type, const char *body);
void server_response_json(Client *client, int status, const char *json);
void server_response_error(Client *client, int status, const char *msg);
void client_close(Client *client);
void client_set_ws_private(Client *client, void *priv);

#endif
