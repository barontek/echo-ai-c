#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "server/websocket.h"
#include "utils/logging.h"

int websocket_test_send_control(WSClient *ws, unsigned char opcode,
                                const unsigned char *payload, size_t payload_len);
int websocket_test_protocol_offered(const char *headers, const char *protocol);

static uv_write_t *captured_req = NULL;
static uv_write_cb captured_cb = NULL;
static char *captured_base = NULL;
static size_t captured_len = 0;
static int submit_result = 0;
static void *captured_ws_private = NULL;

int websocket_test_uv_write(uv_write_t *req, uv_stream_t *handle,
                            const uv_buf_t bufs[], unsigned int nbufs,
                            uv_write_cb cb)
{
    (void)handle;
    ck_assert_uint_eq(nbufs, 1);
    captured_req = req;
    captured_cb = cb;
    captured_base = bufs[0].base;
    captured_len = bufs[0].len;
    return submit_result;
}

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query)
{
    (void)ws;
    (void)ctx;
    (void)query;
}

void client_close(Client *client) { (void)client; }
void client_set_ws_private(Client *client, void *priv)
{
    (void)client;
    captured_ws_private = priv;
}

void log_msg(LogLevel level, const char *file, int line, const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}

/* ---- libuv stubs for ws_do_handshake (never touch real uv internals) ---- */

int uv_read_stop(uv_stream_t *stream) { (void)stream; return 0; }

int uv_read_start(uv_stream_t *stream, uv_alloc_cb alloc_cb, uv_read_cb read_cb)
{
    (void)stream;
    (void)alloc_cb;
    (void)read_cb;
    return 0;
}

uv_loop_t *uv_handle_get_loop(const uv_handle_t *handle) { (void)handle; return (uv_loop_t *)1; }

int uv_timer_init(uv_loop_t *loop, uv_timer_t *timer) { (void)loop; (void)timer; return 0; }

int uv_timer_start(uv_timer_t *timer, uv_timer_cb cb, uint64_t timeout, uint64_t repeat)
{
    (void)timer;
    (void)cb;
    (void)timeout;
    (void)repeat;
    return 0;
}

static void reset_capture(void)
{
    captured_req = NULL;
    captured_cb = NULL;
    captured_base = NULL;
    captured_len = 0;
    submit_result = 0;
    captured_ws_private = NULL;
}

START_TEST(test_control_frame_buffer_survives_until_completion)
{
    reset_capture();
    WSClient ws = {.handle = (uv_tcp_t *)1};
    const unsigned char payload[] = {'o', 'k'};
    ck_assert_int_eq(websocket_test_send_control(&ws, 0xA, payload,
                                                 sizeof(payload)), 0);
    ck_assert_ptr_nonnull(captured_req);
    ck_assert(captured_cb != NULL);
    ck_assert_uint_eq(captured_len, 4);
    ck_assert_uint_eq((unsigned char)captured_base[0], 0x8A);
    ck_assert_uint_eq((unsigned char)captured_base[1], 2);
    ck_assert_mem_eq(captured_base + 2, payload, sizeof(payload));
    captured_cb(captured_req, 0);
    reset_capture();
}
END_TEST

START_TEST(test_close_frame_encodes_requested_code)
{
    reset_capture();
    WSClient ws = {.handle = (uv_tcp_t *)1};
    ck_assert_int_eq(ws_send_close(&ws, 1001), 0);
    ck_assert_uint_eq(captured_len, 4);
    ck_assert_uint_eq((unsigned char)captured_base[0], 0x88);
    ck_assert_uint_eq((unsigned char)captured_base[2], 0x03);
    ck_assert_uint_eq((unsigned char)captured_base[3], 0xE9);
    captured_cb(captured_req, 0);
    reset_capture();
}
END_TEST

START_TEST(test_control_frame_submission_failure_is_reported)
{
    reset_capture();
    submit_result = UV_EPIPE;
    WSClient ws = {.handle = (uv_tcp_t *)1};
    ck_assert_int_eq(websocket_test_send_control(&ws, 0x9, NULL, 0), -1);
    reset_capture();
}
END_TEST

START_TEST(test_protocol_is_selected_only_when_offered)
{
    ck_assert_int_eq(websocket_test_protocol_offered(
        "Sec-WebSocket-Protocol: echo-ai, echo-ai-token-secret\r\n",
        "echo-ai"), 1);
    ck_assert_int_eq(websocket_test_protocol_offered(
        "Host: localhost\r\n", "echo-ai"), 0);
}
END_TEST

START_TEST(test_handshake_upgrades_with_valid_key)
{
    /* Regression: an unconditional `{ return -1; }` after building the 101
     * response made ws_do_handshake reject every connection with 400. */
    reset_capture();
    uv_tcp_t fake_client = {0};  /* Client is opaque; handshake casts it to uv_stream_t */
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    req.client = (Client *)&fake_client;
    snprintf(req.headers, sizeof(req.headers),
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");

    ck_assert_int_eq(ws_do_handshake(&req, (Client *)&fake_client, &ctx), 0);
    ck_assert_ptr_nonnull(captured_base);
    static const char expect[] = "HTTP/1.1 101 Switching Protocols\r\n";
    ck_assert(strncmp(captured_base, expect, sizeof(expect) - 1) == 0);
    /* RFC 6455 §4.2.2 sample: key above + WS_MAGIC -> this accept value. */
    ck_assert(strstr(captured_base, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    captured_cb(captured_req, 0);
    free(captured_ws_private);
    captured_ws_private = NULL;
    reset_capture();
}
END_TEST

START_TEST(test_handshake_rejects_missing_key)
{
    reset_capture();
    uv_tcp_t fake_client = {0};
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    req.client = (Client *)&fake_client;
    snprintf(req.headers, sizeof(req.headers), "Host: localhost\r\n");

    ck_assert_int_eq(ws_do_handshake(&req, (Client *)&fake_client, &ctx), -1);
    reset_capture();
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("WebSocket");
    TCase *tc = tcase_create("Writes");
    tcase_add_test(tc, test_control_frame_buffer_survives_until_completion);
    tcase_add_test(tc, test_close_frame_encodes_requested_code);
    tcase_add_test(tc, test_control_frame_submission_failure_is_reported);
    tcase_add_test(tc, test_protocol_is_selected_only_when_offered);
    suite_add_tcase(suite, tc);
    TCase *tc_handshake = tcase_create("Handshake");
    tcase_add_test(tc_handshake, test_handshake_upgrades_with_valid_key);
    tcase_add_test(tc_handshake, test_handshake_rejects_missing_key);
    suite_add_tcase(suite, tc_handshake);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
