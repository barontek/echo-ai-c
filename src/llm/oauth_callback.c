/*
 * oauth_callback.c - localhost callback server for the interactive
 * login flow: request validation, token exchange, and the callback
 * thread lifecycle.
 * Depends on: sockets, pthread, oauth_vault, oauth_http.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "oauth_callback.h"
#include "oauth_codec.h"
#include "oauth_vault.h"
#include "oauth_http.h"
#include "openai_oauth_internal.h"
#include "../utils/logging.h"

static const unsigned char *bytes_find(const unsigned char *data, size_t len,
                                       const char *needle, size_t needle_len)
{
    if (!data || !needle || needle_len == 0 || needle_len > len) return NULL;
    for (size_t index = 0; index <= len - needle_len; index++)
        if (memcmp(data + index, needle, needle_len) == 0) return data + index;
    return NULL;
}

static int valid_header_name(const unsigned char *name, size_t len)
{
    if (!name || len == 0) return 0;
    for (size_t index = 0; index < len; index++)
    {
        unsigned char value = name[index];
        if (!isalnum(value) && value != '!' && value != '#' && value != '$' &&
            value != '%' && value != '&' && value != '\'' && value != '*' &&
            value != '+' && value != '-' && value != '.' && value != '^' &&
            value != '_' && value != '`' && value != '|' && value != '~') return 0;
    }
    return 1;
}

static int name_equal(const unsigned char *name, size_t len, const char *wanted)
{
    size_t wanted_len = strlen(wanted);
    if (len != wanted_len) return 0;
    for (size_t index = 0; index < len; index++)
        if (tolower(name[index]) != tolower((unsigned char)wanted[index])) return 0;
    return 1;
}

static int host_value_valid(const unsigned char *value, size_t len)
{
    while (len > 0 && (*value == ' ' || *value == '\t')) {
        value++;
        len--;
    }
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) len--;
    const char *localhost = "localhost:1455";
    const char *loopback = "127.0.0.1:1455";
    if (len == strlen(loopback) && memcmp(value, loopback, len) == 0) return 1;
    if (len != strlen(localhost)) return 0;
    for (size_t index = 0; index < len; index++)
        if (tolower(value[index]) != tolower((unsigned char)localhost[index])) return 0;
    return 1;
}

static int validate_callback_headers(const unsigned char *headers, size_t len)
{
    size_t offset = 0;
    int host_count = 0;
    while (offset < len)
    {
        const unsigned char *end = bytes_find(headers + offset, len - offset, "\r\n", 2);
        if (!end) return -1;
        size_t line_len = (size_t)(end - (headers + offset));
        if (line_len == 0) return offset + 2 == len && host_count == 1 ? 0 : -1;
        const unsigned char *colon = memchr(headers + offset, ':', line_len);
        if (!colon) return -1;
        size_t name_len = (size_t)(colon - (headers + offset));
        if (!valid_header_name(headers + offset, name_len)) return -1;
        if (name_equal(headers + offset, name_len, "content-length") ||
            name_equal(headers + offset, name_len, "transfer-encoding")) return -1;
        if (name_equal(headers + offset, name_len, "host"))
        {
            host_count++;
            size_t value_offset = name_len + 1;
            if (host_count > 1 || !host_value_valid(headers + offset + value_offset,
                                                     line_len - value_offset)) return -1;
        }
        for (size_t index = name_len + 1; index < line_len; index++)
        {
            unsigned char value = headers[offset + index];
            if ((value < 0x20 && value != '\t') || value == 0x7f) return -1;
        }
        offset += line_len + 2;
    }
    return -1;
}

static int callback_set_field(OAuthCallback *callback, char **names, size_t count,
                              char *name, char *value)
{
    for (size_t index = 0; index < count; index++)
        if (strcmp(names[index], name) == 0) return -1;
    if (strcmp(name, "code") == 0) callback->code = value;
    else if (strcmp(name, "state") == 0) callback->state = value;
    else if (strcmp(name, "error") == 0) callback->denial = value;
    else free(value);
    return 0;
}

static void callback_clear(OAuthCallback *callback)
{
    if (!callback) return;
    secure_free(&callback->code);
    secure_free(&callback->state);
    secure_free(&callback->denial);
}

static int parse_callback_query(const unsigned char *query, size_t len,
                                OAuthCallback *callback)
{
    char *names[OAUTH_QUERY_FIELDS_MAX] = {0};
    size_t count = 0;
    size_t offset = 0;
    int result = -1;
    while (offset < len && count < OAUTH_QUERY_FIELDS_MAX)
    {
        const unsigned char *amp = memchr(query + offset, '&', len - offset);
        size_t part_len = amp ? (size_t)(amp - (query + offset)) : len - offset;
        const unsigned char *equals = memchr(query + offset, '=', part_len);
        if (!equals || equals == query + offset) goto cleanup;
        size_t name_len = (size_t)(equals - (query + offset));
        char *name = url_decode_exact(query + offset, name_len);
        char *value = url_decode_exact(equals + 1, part_len - name_len - 1);
        if (!name || !value) {
            free(name);
            secure_free(&value);
            goto cleanup;
        }
        names[count] = name;
        if (callback_set_field(callback, names, count, name, value) != 0)
         {
            free(name);
            names[count] = NULL;
            secure_free(&value);
            goto cleanup;
        }
        count++;
        if (!amp) {
            offset = len;
            break;
        }
        offset += part_len + 1;
        if (offset == len) goto cleanup;
    }
    if (offset != len || !callback->state) goto cleanup;
    if ((!callback->code && !callback->denial) || (callback->code && callback->denial))
        goto cleanup;
    result = 0;
cleanup:
    for (size_t index = 0; index < count; index++) free(names[index]);
    if (result != 0) callback_clear(callback);
    return result;
}

int parse_callback_request(const void *request, size_t request_len,
                                  OAuthCallback *callback)
{
    if (!request || !callback || request_len == 0 || request_len > OAUTH_REQUEST_MAX)
        return -1;
    memset(callback, 0, sizeof(*callback));
    const unsigned char *data = request;
    if (memchr(data, '\0', request_len)) return -1;
    const unsigned char *header_end = bytes_find(data, request_len, "\r\n\r\n", 4);
    if (!header_end || (size_t)(header_end - data) + 4 != request_len) return -1;
    const unsigned char *line_end = bytes_find(data, request_len, "\r\n", 2);
    if (!line_end) return -1;
    size_t line_len = (size_t)(line_end - data);
    const unsigned char *first_space = memchr(data, ' ', line_len);
    if (!first_space || (size_t)(first_space - data) != 3 || memcmp(data, "GET", 3) != 0)
        return -1;
    size_t remaining = line_len - 4;
    const unsigned char *target = first_space + 1;
    const unsigned char *second_space = memchr(target, ' ', remaining);
    if (!second_space || memchr(second_space + 1, ' ',
                                (size_t)(line_end - second_space - 1))) return -1;
    size_t target_len = (size_t)(second_space - target);
    size_t version_len = (size_t)(line_end - second_space - 1);
    if (version_len != 8 || memcmp(second_space + 1, "HTTP/1.1", 8) != 0) return -1;
    size_t path_len = strlen(OPENAI_CALLBACK_PATH);
    if (target_len <= path_len || memcmp(target, OPENAI_CALLBACK_PATH, path_len) != 0 ||
        target[path_len] != '?') return -1;
    if (memchr(target, '#', target_len)) return -1;
    const unsigned char *headers = line_end + 2;
    size_t headers_len = request_len - (size_t)(headers - data);
    if (validate_callback_headers(headers, headers_len) != 0) return -1;
    return parse_callback_query(target + path_len + 1,
                                target_len - path_len - 1, callback);
}

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
#ifdef MSG_NOSIGNAL
        ssize_t count = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t count = send(fd, data + sent, len - sent, 0);
#endif
        if (count > 0) sent += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int callback_response(int fd, int status, const char *body)
{
    const char *reason = status == 200 ? "OK" : "Bad Request";
    char header[256] = {0};
    size_t body_len = strlen(body);
    int len = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\n"
                       "Content-Type: text/plain; charset=utf-8\r\n"
                       "Connection: close\r\nContent-Length: %zu\r\n\r\n",
                       status, reason, body_len);
    if (len < 0 || (size_t)len >= sizeof(header)) return -1;
    if (send_all(fd, header, (size_t)len) != 0) return -1;
    return send_all(fd, body, body_len);
}

static int callback_still_active(OpenAIOAuth *auth, uint64_t generation)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return 0;
    int active = !auth->destroying && !auth->stop_requested &&
                 auth->generation == generation;
    if (pthread_mutex_unlock(&auth->lock) != 0) return 0;
    return active;
}

static int read_callback_request(OpenAIOAuth *auth, uint64_t generation, int fd,
                                 unsigned char *request, size_t *request_len)
{
    size_t used = 0;
    time_t deadline = time(NULL) + OAUTH_CLIENT_TIMEOUT_SECONDS;
    while (used <= OAUTH_REQUEST_MAX && callback_still_active(auth, generation))
    {
        const unsigned char *end = bytes_find(request, used, "\r\n\r\n", 4);
        if (end) {
            *request_len = used;
            return 0;
        }
        if (used == OAUTH_REQUEST_MAX) return -1;
        time_t now = time(NULL);
        if (now == (time_t)-1 || now >= deadline) return -1;
        struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
        int selected = poll(&descriptor, 1, 1000);
        if (selected < 0 && errno == EINTR) continue;
        if (selected < 0) return -1;
        if (selected == 0) continue;
        ssize_t count = recv(fd, request + used, OAUTH_REQUEST_MAX - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        if (memchr(request + used, '\0', (size_t)count)) return -1;
        used += (size_t)count;
    }
    return -1;
}

int state_matches(const char *expected, const char *actual)
{
    if (!expected || !actual) return 0;
    size_t expected_len = strlen(expected);
    size_t actual_len = strlen(actual);
    return expected_len == actual_len &&
           CRYPTO_memcmp(expected, actual, expected_len) == 0;
}

int complete_callback_tokens(OpenAIOAuth *auth, uint64_t generation,
                                    const char *json)
{
    OAuthCredentials staged = {0};
    time_t now = time(NULL);
    if (now == (time_t)-1 || token_set_parse(json, NULL, NULL, NULL, now, &staged) != 0)
        return -1;
    int result = -1;
    if (pthread_mutex_lock(&auth->lock) == 0)
    {
        if (auth->generation == generation && !auth->stop_requested && auth->session &&
            persist_staged_locked(auth, &staged) == 0)
        {
            commit_credentials_locked(auth, &staged);
            secure_free(&auth->last_error);
            result = 0;
        }
        else if (auth->generation == generation && !auth->stop_requested)
            set_error_locked(auth, "Could not save OpenAI credentials");
        (void)pthread_mutex_unlock(&auth->lock);
    }
    credentials_clear(&staged);
    return result;
}

static int process_callback(OpenAIOAuth *auth, uint64_t generation,
                            const unsigned char *request, size_t request_len)
{
    OAuthCallback callback = {0};
    if (parse_callback_request(request, request_len, &callback) != 0)
    {
        if (pthread_mutex_lock(&auth->lock) == 0)
        {
            if (auth->generation == generation)
                set_error_locked(auth, "Invalid OpenAI OAuth callback");
            (void)pthread_mutex_unlock(&auth->lock);
        }
        return -2;
    }
    char *verifier = NULL;
    int valid_state = 0;
    if (pthread_mutex_lock(&auth->lock) == 0)
    {
        valid_state = auth->generation == generation && !auth->stop_requested &&
                      state_matches(auth->state, callback.state);
        if (valid_state && !callback.denial) verifier = string_dup(auth->verifier);
        if (valid_state) clear_pending_sensitive_locked(auth);
        if (!valid_state) set_error_locked(auth, "Invalid OpenAI OAuth state");
        else if (callback.denial) set_error_locked(auth, "OpenAI login was denied");
        (void)pthread_mutex_unlock(&auth->lock);
    }
    int result = -1;
    if (valid_state && !callback.denial && verifier)
    {
        char *json = NULL;
        OpenAIOAuthTokenResult exchange = exchange_token(auth, generation,
            "authorization_code", callback.code, OPENAI_REDIRECT_URI, verifier, &json);
        if (exchange == OPENAI_OAUTH_TOKEN_OK)
            result = complete_callback_tokens(auth, generation, json);
        else if (exchange != OPENAI_OAUTH_TOKEN_CANCELLED &&
                 pthread_mutex_lock(&auth->lock) == 0)
        {
            if (auth->generation == generation)
                set_error_locked(auth, "OpenAI token exchange failed");
            (void)pthread_mutex_unlock(&auth->lock);
        }
        if (json) {
            OPENSSL_cleanse(json, strlen(json));
            free(json);
        }
    }
    else if (valid_state && !callback.denial && !verifier &&
             pthread_mutex_lock(&auth->lock) == 0)
    {
        set_error_locked(auth, "OpenAI OAuth state could not be retained");
        (void)pthread_mutex_unlock(&auth->lock);
    }
    secure_free(&verifier);
    callback_clear(&callback);
    return valid_state ? result : -2;
}

static void callback_publish_failure(OpenAIOAuth *auth, uint64_t generation)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->generation == generation)
    {
        auth->callback_ready = 1;
        auth->callback_rc = -1;
        (void)pthread_cond_broadcast(&auth->condition);
    }
    (void)pthread_mutex_unlock(&auth->lock);
}

static int callback_publish_listener(OpenAIOAuth *auth, uint64_t generation, int fd)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    int valid = auth->generation == generation && !auth->stop_requested && !auth->destroying;
    if (valid)
    {
        auth->listener_fd = fd;
        auth->callback_ready = 1;
        auth->callback_rc = 0;
        (void)pthread_cond_broadcast(&auth->condition);
    }
    (void)pthread_mutex_unlock(&auth->lock);
    return valid ? 0 : -1;
}

static void callback_finish(OpenAIOAuth *auth, uint64_t generation, int timed_out)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->generation == generation)
    {
        if (timed_out && !auth->stop_requested)
            set_error_locked(auth, "OpenAI login timed out");
        auth->callback_active = 0;
        auth->listener_fd = -1;
        auth->client_fd = -1;
        clear_pending_sensitive_locked(auth);
        (void)pthread_cond_broadcast(&auth->condition);
    }
    (void)pthread_mutex_unlock(&auth->lock);
}

static int wait_for_callback_client(OpenAIOAuth *auth, uint64_t generation, int fd,
                                    time_t deadline, int *timed_out)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
    for (;;)
    {
        time_t now = time(NULL);
        if (now == (time_t)-1 || now >= deadline)
         {
            *timed_out = 1;
            return -1;
        }
        time_t remaining = deadline - now;
        int timeout_ms = remaining > INT_MAX / 1000 ? INT_MAX : (int)remaining * 1000;
        /* Bound each poll slice so cancellation is noticed promptly even on
         * platforms where shutdown() does not wake poll() on a listening
         * socket; an unbounded wait would block join/cancel for the whole
         * login timeout. */
        if (timeout_ms > OAUTH_POLL_SLICE_MS) timeout_ms = OAUTH_POLL_SLICE_MS;
        int selected = poll(&descriptor, 1, timeout_ms);
        if (selected > 0 && callback_still_active(auth, generation))
            return accept(fd, NULL, NULL);
        if (!callback_still_active(auth, generation)) return -1;
        if (selected < 0 && errno != EINTR) return -1;
    }
}

static void untrack_socket(OpenAIOAuth *auth, int fd, int client_socket)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    int *tracked = client_socket ? &auth->client_fd : &auth->listener_fd;
    if (*tracked == fd) *tracked = -1;
    (void)pthread_mutex_unlock(&auth->lock);
}

void *callback_thread_main(void *userdata)
{
    OAuthThreadArgs *args = userdata;
    OpenAIOAuth *auth = args->auth;
    uint64_t generation = args->generation;
    free(args);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int timed_out = 0;
    if (listener < 0) {
        callback_publish_failure(auth, generation);
        goto finished;
    }
    int reuse = 1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(OPENAI_CALLBACK_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0 ||
        callback_publish_listener(auth, generation, listener) != 0)
     {
        callback_publish_failure(auth, generation);
        goto close_listener;
    }
    time_t now = time(NULL);
    if (now == (time_t)-1) goto close_listener;
    time_t deadline = now + OAUTH_LOGIN_TIMEOUT_SECONDS;
    while (callback_still_active(auth, generation))
    {
        int client = wait_for_callback_client(auth, generation, listener,
                                              deadline, &timed_out);
        if (client < 0) break;
        if (pthread_mutex_lock(&auth->lock) == 0)
        {
            if (auth->generation == generation) auth->client_fd = client;
            (void)pthread_mutex_unlock(&auth->lock);
        }
        unsigned char request[OAUTH_REQUEST_MAX] = {0};
        size_t request_len = 0;
        int result = -2;
        if (read_callback_request(auth, generation, client, request, &request_len) == 0)
            result = process_callback(auth, generation, request, request_len);
        else if (pthread_mutex_lock(&auth->lock) == 0)
        {
            if (auth->generation == generation && !auth->stop_requested)
                set_error_locked(auth, "Invalid or oversized OpenAI OAuth callback");
            (void)pthread_mutex_unlock(&auth->lock);
        }
        OPENSSL_cleanse(request, sizeof(request));
        if (callback_response(client, result == 0 ? 200 : 400,
            result == 0 ? "OpenAI login complete. You may close this window."
                        : "OpenAI login failed. You may close this window.") != 0)
            log_error("send OpenAI OAuth callback response", NULL);
        untrack_socket(auth, client, 1);
        if (close(client) != 0) log_error("close OpenAI OAuth client socket", NULL);
        if (result != -2) break;
    }
close_listener:
    untrack_socket(auth, listener, 0);
    if (close(listener) != 0) log_error("close OpenAI OAuth listener socket", NULL);
finished:
    callback_finish(auth, generation, timed_out);
    return NULL;
}

void shutdown_fd(int fd)
{
    if (fd >= 0 && shutdown(fd, SHUT_RDWR) != 0 && errno != ENOTCONN && errno != EINVAL)
        log_error("shutdown OpenAI OAuth socket", NULL);
}

void cancel_callback_locked(OpenAIOAuth *auth)
{
    auth->generation = next_generation(auth->generation);
    auth->stop_requested = 1;
    shutdown_fd(auth->client_fd);
    shutdown_fd(auth->listener_fd);
    clear_pending_sensitive_locked(auth);
    auth->callback_active = 0;
}

int take_callback_thread_locked(OpenAIOAuth *auth, pthread_t *thread)
{
    if (!auth->thread_joinable) return 0;
    *thread = auth->callback_thread;
    auth->thread_joinable = 0;
    return 1;
}

int join_callback_thread(pthread_t thread, int joinable)
{
    return !joinable || pthread_join(thread, NULL) == 0 ? 0 : -1;
}

void lifecycle_finish(OpenAIOAuth *auth)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    auth->lifecycle_busy = 0;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
}

void active_operation_finish(OpenAIOAuth *auth)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->active_operations > 0) auth->active_operations--;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
}

int reap_previous_callback(OpenAIOAuth *auth)
{
    pthread_t thread = {0};
    int joinable = 0;
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    if (auth->callback_active || auth->destroying || auth->lifecycle_busy)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    return join_callback_thread(thread, joinable);
}
