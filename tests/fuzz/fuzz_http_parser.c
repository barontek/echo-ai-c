#include <stddef.h>
#include <stdint.h>
#include "server/server.h"
#include "server/routes/routes.h"
#include "server/websocket.h"
#include "utils/logging.h"

int server_test_parse_chunks(const char **chunks, const size_t *lengths, int count,
                             char *method, size_t method_size,
                             char *path, size_t path_size,
                             char *body, size_t body_size);

const Route routes[] = {{0}};
const int routes_count = 0;
int route_match(const char *method, const char *path, const Route *route)
{ (void)method; (void)path; (void)route; return 0; }
int ws_do_handshake(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; return -1; }
int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx)
{ (void)req; (void)ctx; return 0; }
int middleware_check_unlock_query(HTTPRequest *req, ServerContext *ctx)
{ (void)req; (void)ctx; return 0; }
int middleware_has_valid_ws_token(const char *headers, const char *token)
{ (void)headers; (void)token; return 0; }
int rate_limiter_allow(RateLimiter *limiter, const char *key)
{ (void)limiter; (void)key; return 1; }
void log_msg(LogLevel level, const char *file, int line, const char *message, ...)
{ (void)level; (void)file; (void)line; (void)message; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 12U * 1024U * 1024U) return 0;
    size_t split = size / 2;
    const char *chunks[] = {(const char *)data, (const char *)data + split};
    size_t lengths[] = {split, size - split};
    char method[16] = {0};
    char path[1024] = {0};
    char body[64] = {0};
    (void)server_test_parse_chunks(chunks, lengths, 2,
                                   method, sizeof(method),
                                   path, sizeof(path),
                                   body, sizeof(body));
    return 0;
}
