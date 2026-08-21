/*
 * openai_oauth_internal.h - shared state and cross-unit contracts for the
 * OpenAI OAuth manager. Documented exception to the one-header-per-module
 * rule: the manager's state machine is spread across oauth_codec/jwt/vault/
 * http/callback/device units that all mutate one private struct, so the
 * struct and constants live here instead of being duplicated. Function
 * contracts live in the unit headers (oauth_codec.h, oauth_jwt.h, ...).
 * Not installed or included outside src/llm.
 * Depends on: openai_oauth.h (public API types), pthread, cJSON.
 */

#ifndef ECHO_OPENAI_OAUTH_INTERNAL_H
#define ECHO_OPENAI_OAUTH_INTERNAL_H

#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "openai_oauth.h"

#define OPENAI_ISSUER "https://auth.openai.com"
#define OPENAI_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define OPENAI_CALLBACK_PORT 1455
#define OPENAI_CALLBACK_PATH "/auth/callback"
#define OPENAI_REDIRECT_URI "http://localhost:1455/auth/callback"
#define OPENAI_DEVICE_REDIRECT_URI "https://auth.openai.com/deviceauth/callback"
#define OPENAI_DEVICE_VERIFICATION_URL "https://auth.openai.com/codex/device"
#define OPENAI_PROVIDER_NAME "openai"
#define OAUTH_LOGIN_TIMEOUT_SECONDS 300
#define OAUTH_POLL_SLICE_MS 1000
#define OAUTH_CLIENT_TIMEOUT_SECONDS 10
#define OAUTH_REFRESH_SKEW_SECONDS 300
#define OAUTH_REQUEST_MAX 8192
#define OAUTH_RESPONSE_MAX ((size_t)1024U * 1024U)
#define OAUTH_VALUE_MAX ((size_t)256U * 1024U)
#define OAUTH_QUERY_FIELDS_MAX 16

/* Staged credentials parsed from a token endpoint response or stored row;
 * every pointer is caller-or-unit owned and must be freed with
 * credentials_clear(). */
typedef struct {
    char *access_token;
    char *refresh_token;
    char *account_id;
    char *plan_type;
    time_t expires_at;
} OAuthCredentials;

/* Parsed OAuth callback query: code+state on success, denial on failure. */
typedef struct {
    char *code;
    char *state;
    char *denial;
} OAuthCallback;

/* Thread entry argument for the callback server; freed by the thread. */
typedef struct {
    struct OpenAIOAuth *auth;
    uint64_t generation;
} OAuthThreadArgs;

/* Cancellation context passed to curl's progress callback. */
typedef struct {
    struct OpenAIOAuth *auth;
    uint64_t generation;
} OAuthCancelContext;

struct OpenAIOAuth {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t callback_thread;
    int thread_joinable;
    int callback_active;
    int callback_ready;
    int callback_rc;
    int listener_fd;
    int client_fd;
    int stop_requested;
    int destroying;
    int lifecycle_busy;
    int active_operations;
    int refresh_in_progress;
    OpenAIOAuthTokenResult last_refresh_result;
    uint64_t generation;
    uint64_t token_version;
    char *state;
    char *verifier;
    char *challenge;
    char *login_id;
    char *device_auth_id;
    char *device_user_code;
    time_t device_expires_at;
    time_t device_next_poll;
    unsigned int device_interval;
    int device_poll_in_progress;
    char *access_token;
    char *refresh_token;
    char *account_id;
    char *plan_type;
    char *last_error;
    time_t expires_at;
    SessionManager *session;
};

/* openai_oauth.c (core) - error recording. Requires the manager lock;
 * frees the previous error string and dups @message. */
void set_error_locked(OpenAIOAuth *auth, const char *message);

#endif /* ECHO_OPENAI_OAUTH_INTERNAL_H */
