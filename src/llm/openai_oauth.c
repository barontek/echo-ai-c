/*
 * openai_oauth.c - OAuth manager for the ChatGPT Codex provider:
 * state machine, public API, and single-flight token refresh.
 * Split across oauth_codec/jwt/vault/http/callback/device units;
 * see openai_oauth_internal.h for the shared struct.
 * Depends on: libcurl, cJSON, OpenSSL, pthread, SessionManager.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>

#include "openai_oauth.h"
#include "openai_oauth_internal.h"
#include "oauth_codec.h"
#include "oauth_jwt.h"
#include "oauth_vault.h"
#include "oauth_http.h"
#include "oauth_callback.h"
#include "oauth_device.h"
#include "../utils/logging.h"


void set_error_locked(OpenAIOAuth *auth, const char *message)
{
    secure_free(&auth->last_error);
    auth->last_error = string_dup(message ? message : "OpenAI OAuth failed");
}

OpenAIOAuth *openai_oauth_create(void)
{
    OpenAIOAuth *auth = calloc(1, sizeof(*auth));
    if (!auth) return NULL;
    if (pthread_mutex_init(&auth->lock, NULL) != 0)
     {
        free(auth);
        return NULL;
    }
    if (pthread_cond_init(&auth->condition, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&auth->lock);
        free(auth);
        return NULL;
    }
    auth->listener_fd = -1;
    auth->client_fd = -1;
    auth->last_refresh_result = OPENAI_OAUTH_TOKEN_SIGNED_OUT;
    return auth;
}

void openai_oauth_destroy(OpenAIOAuth *auth)
{
    if (!auth) return;
    pthread_t thread = {0};
    int joinable = 0;
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    auth->destroying = 1;
    cancel_callback_locked(auth);
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    (void)join_callback_thread(thread, joinable);
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    while (auth->refresh_in_progress || auth->active_operations ||
           auth->lifecycle_busy)
        if (pthread_cond_wait(&auth->condition, &auth->lock) != 0) break;
    clear_pending_sensitive_locked(auth);
    secure_free(&auth->login_id);
    clear_credentials_locked(auth);
    secure_free(&auth->last_error);
    auth->session = NULL;
    (void)pthread_mutex_unlock(&auth->lock);
    (void)pthread_cond_destroy(&auth->condition);
    (void)pthread_mutex_destroy(&auth->lock);
    free(auth);
}

int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *session)
{
    if (!auth) return -1;
    pthread_t thread = {0};
    int joinable = 0;
    if (pthread_mutex_lock(&auth->lock) != 0)
        return -1;
    if (auth->destroying || auth->lifecycle_busy)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    auth->lifecycle_busy = 1;
    cancel_callback_locked(auth);
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    if (join_callback_thread(thread, joinable) != 0)
     {
        lifecycle_finish(auth);
        return -1;
    }
    if (pthread_mutex_lock(&auth->lock) != 0)
        return -1;
    while (auth->refresh_in_progress || auth->active_operations)
        if (pthread_cond_wait(&auth->condition, &auth->lock) != 0)
        { auth->lifecycle_busy = 0; (void)pthread_cond_broadcast(&auth->condition);
          (void)pthread_mutex_unlock(&auth->lock); return -1; }
    auth->session = NULL;
    clear_credentials_locked(auth);
    secure_free(&auth->login_id);
    (void)pthread_mutex_unlock(&auth->lock);

    char *data = NULL;
    ProviderOAuthLoadResult load_result = session ?
        session_manager_load_provider_oauth_ex(session, OPENAI_PROVIDER_NAME,
                                               &data) :
        PROVIDER_OAUTH_LOAD_NOT_FOUND;
    OAuthCredentials staged = {0};
    int parse_result = load_result == PROVIDER_OAUTH_LOAD_NOT_FOUND ? 0 :
        (load_result == PROVIDER_OAUTH_LOAD_OK && data ?
             stored_credentials_parse(data, &staged) : -1);
    if (data) {
        OPENSSL_cleanse(data, strlen(data));
        free(data);
    }
    if (pthread_mutex_lock(&auth->lock) != 0)
     {
        credentials_clear(&staged);
        return -1;
    }
    if (auth->destroying)
    { auth->lifecycle_busy = 0; (void)pthread_cond_broadcast(&auth->condition);
      (void)pthread_mutex_unlock(&auth->lock); credentials_clear(&staged); return -1; }
    auth->session = session;
    if (parse_result == 0 && staged.access_token) commit_credentials_locked(auth, &staged);
    if (parse_result != 0) set_error_locked(auth, "Stored OpenAI credentials are invalid");
    else secure_free(&auth->last_error);
    auth->stop_requested = 0;
    auth->lifecycle_busy = 0;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
    credentials_clear(&staged);
    return parse_result;
}

int openai_oauth_start(OpenAIOAuth *auth, char **authorization_url, char **login_id)
{
    if (!auth || !authorization_url || !login_id) return -1;
    *authorization_url = NULL;
    *login_id = NULL;
    if (reap_previous_callback(auth) != 0) return -1;
    char *state = NULL;
    char *verifier = NULL;
    char *challenge = NULL;
    char *id = NULL;
    if (random_string(&state) != 0 || make_pkce(&verifier, &challenge) != 0 ||
        random_string(&id) != 0) goto cleanup;
    char *url = build_authorize_url_values(state, challenge);
    char *id_copy = string_dup(id);
    OAuthThreadArgs *args = malloc(sizeof(*args));
    if (!url || !id_copy || !args) {
        free(url);
        free(id_copy);
        free(args);
        goto cleanup;
    }
    if (pthread_mutex_lock(&auth->lock) != 0)
     {
        free(url);
        free(id_copy);
        free(args);
        goto cleanup;
    }
    if (!auth->session || auth->callback_active || auth->destroying)
    {
        (void)pthread_mutex_unlock(&auth->lock);
        free(url); free(id_copy); free(args);
        goto cleanup;
    }
    clear_pending_sensitive_locked(auth);
    secure_free(&auth->login_id);
    auth->state = state; state = NULL;
    auth->verifier = verifier; verifier = NULL;
    auth->challenge = challenge; challenge = NULL;
    auth->login_id = id; id = NULL;
    secure_free(&auth->last_error);
    auth->generation = next_generation(auth->generation);
    auth->stop_requested = 0;
    auth->callback_active = 1;
    auth->callback_ready = 0;
    auth->callback_rc = -1;
    args->auth = auth;
    args->generation = auth->generation;
    int create_result = pthread_create(&auth->callback_thread, NULL,
                                       callback_thread_main, args);
    if (create_result != 0)
    {
        auth->callback_active = 0;
        clear_pending_sensitive_locked(auth);
        secure_free(&auth->login_id);
        (void)pthread_mutex_unlock(&auth->lock);
        free(args); free(url); free(id_copy);
        goto cleanup;
    }
    auth->thread_joinable = 1;
    while (!auth->callback_ready)
    {
        if (pthread_cond_wait(&auth->condition, &auth->lock) != 0)
         {
            auth->callback_rc = -1;
            break;
        }
    }
    int ready = auth->callback_rc == 0;
    pthread_t thread = auth->callback_thread;
    if (!ready)
    {
        auth->thread_joinable = 0;
        auth->stop_requested = 1;
        shutdown_fd(auth->listener_fd);
        shutdown_fd(auth->client_fd);
    }
    (void)pthread_mutex_unlock(&auth->lock);
    if (!ready)
    {
        if (pthread_join(thread, NULL) != 0)
            log_error("join OpenAI OAuth callback thread", NULL);
        if (pthread_mutex_lock(&auth->lock) == 0)
        {
            clear_pending_sensitive_locked(auth);
            secure_free(&auth->login_id);
            auth->stop_requested = 0;
            (void)pthread_mutex_unlock(&auth->lock);
        }
        free(url); free(id_copy);
        goto cleanup;
    }
    *authorization_url = url;
    *login_id = id_copy;
    return 0;
cleanup:
    secure_free(&state);
    secure_free(&verifier);
    secure_free(&challenge);
    secure_free(&id);
    return -1;
}

static OpenAIOAuthState status_locked(const OpenAIOAuth *auth)
{
    if (auth->callback_active) return OPENAI_OAUTH_PENDING;
    return auth->refresh_token ? OPENAI_OAUTH_SIGNED_IN : OPENAI_OAUTH_SIGNED_OUT;
}

static int copy_status_locked(OpenAIOAuth *auth, char **account_id,
                              char **plan_type, char **error)
{
    if (account_id && auth->account_id)
    {
        *account_id = string_dup(auth->account_id);
        if (!*account_id) return -1;
    }
    if (plan_type && auth->plan_type)
    {
        *plan_type = string_dup(auth->plan_type);
        if (!*plan_type) goto failed;
    }
    if (error && auth->last_error)
    {
        *error = string_dup(auth->last_error);
        if (!*error) goto failed;
    }
    return 0;
failed:
    if (account_id) secure_free(account_id);
    if (plan_type) secure_free(plan_type);
    return -1;
}

OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error)
{
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    if (!auth || pthread_mutex_lock(&auth->lock) != 0) return OPENAI_OAUTH_SIGNED_OUT;
    OpenAIOAuthState state = status_locked(auth);
    (void)copy_status_locked(auth, account_id, plan_type, error);
    (void)pthread_mutex_unlock(&auth->lock);
    return state;
}

int openai_oauth_status_for_login(OpenAIOAuth *auth, const char *login_id,
                                  OpenAIOAuthState *state, char **account_id,
                                  char **plan_type, char **error)
{
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    if (!auth || !login_id || !state || pthread_mutex_lock(&auth->lock) != 0) return -1;
    int valid = auth->login_id && state_matches(auth->login_id, login_id);
    int result = -1;
    if (valid)
    {
        *state = status_locked(auth);
        result = copy_status_locked(auth, account_id, plan_type, error);
    }
    (void)pthread_mutex_unlock(&auth->lock);
    return result;
}

int openai_oauth_cancel_login(OpenAIOAuth *auth, const char *login_id)
{
    if (!auth || !login_id) return -1;
    pthread_t thread = {0};
    int joinable = 0;
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    if (!auth->callback_active || !auth->login_id ||
        !state_matches(auth->login_id, login_id) || auth->destroying ||
        auth->lifecycle_busy)
    {
        (void)pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    auth->lifecycle_busy = 1;
    cancel_callback_locked(auth);
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    if (join_callback_thread(thread, joinable) != 0)
     {
        lifecycle_finish(auth);
        return -1;
    }
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    while (auth->active_operations || auth->refresh_in_progress)
        if (pthread_cond_wait(&auth->condition, &auth->lock) != 0)
        { auth->lifecycle_busy = 0; (void)pthread_cond_broadcast(&auth->condition);
          (void)pthread_mutex_unlock(&auth->lock); return -1; }
    secure_free(&auth->login_id);
    secure_free(&auth->last_error);
    auth->stop_requested = 0;
    auth->lifecycle_busy = 0;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
    return 0;
}

static int copy_access_locked(OpenAIOAuth *auth, char **access_token,
                              char **account_id)
{
    *access_token = string_dup(auth->access_token);
    if (!*access_token) return -1;
    if (auth->account_id)
    {
        *account_id = string_dup(auth->account_id);
        if (!*account_id) {
            secure_free(access_token);
            return -1;
        }
    }
    return 0;
}

static OpenAIOAuthTokenResult refresh_credentials(OpenAIOAuth *auth, int force,
                                                   char **access_token,
                                                   char **account_id)
{
    uint64_t observed_version = auth->token_version;
    int waited = 0;
    while (auth->refresh_in_progress)
    {
        waited = 1;
        if (pthread_cond_wait(&auth->condition, &auth->lock) != 0)
            return OPENAI_OAUTH_TOKEN_TRANSIENT;
        if (!auth->refresh_token || !auth->session || auth->stop_requested ||
            auth->destroying)
            return auth->last_refresh_result;
        if (force && auth->token_version != observed_version)
            return copy_access_locked(auth, access_token, account_id) == 0 ?
                OPENAI_OAUTH_TOKEN_OK : OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    if (waited)
    {
        if (auth->last_refresh_result != OPENAI_OAUTH_TOKEN_OK)
            return auth->last_refresh_result;
        return copy_access_locked(auth, access_token, account_id) == 0 ?
            OPENAI_OAUTH_TOKEN_OK : OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    time_t now = time(NULL);
    if (now == (time_t)-1) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    if (!force && !needs_refresh(auth->expires_at, now))
        return copy_access_locked(auth, access_token, account_id) == 0 ?
            OPENAI_OAUTH_TOKEN_OK : OPENAI_OAUTH_TOKEN_TRANSIENT;
    char *refresh = string_dup(auth->refresh_token);
    char *old_account = auth->account_id ? string_dup(auth->account_id) : NULL;
    char *old_plan = auth->plan_type ? string_dup(auth->plan_type) : NULL;
    if (!refresh || (auth->account_id && !old_account) || (auth->plan_type && !old_plan))
    {
        secure_free(&refresh);
        secure_free(&old_account);
        secure_free(&old_plan);
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    uint64_t generation = auth->generation;
    auth->refresh_in_progress = 1;
    if (pthread_mutex_unlock(&auth->lock) != 0)
    {
        secure_free(&refresh);
        secure_free(&old_account);
        secure_free(&old_plan);
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    char *json = NULL;
    OpenAIOAuthTokenResult result = exchange_token(auth, generation, "refresh_token",
                                                    refresh, NULL, NULL, &json);
    OAuthCredentials staged = {0};
    now = time(NULL);
    if (result == OPENAI_OAUTH_TOKEN_OK &&
        (now == (time_t)-1 || token_set_parse(json, refresh, old_account, old_plan,
                                              now, &staged) != 0))
        result = OPENAI_OAUTH_TOKEN_TRANSIENT;
    if (json) {
        OPENSSL_cleanse(json, strlen(json));
        free(json);
    }
    secure_free(&refresh);
    secure_free(&old_account);
    secure_free(&old_plan);
    if (pthread_mutex_lock(&auth->lock) != 0)
     {
        credentials_clear(&staged);
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    if (auth->generation != generation || auth->stop_requested || auth->destroying)
        result = OPENAI_OAUTH_TOKEN_CANCELLED;
    else if (result == OPENAI_OAUTH_TOKEN_OK)
    {
        if (persist_staged_locked(auth, &staged) == 0)
        {
            commit_credentials_locked(auth, &staged);
            secure_free(&auth->last_error);
        }
        else
        {
            set_error_locked(auth, "Could not save refreshed OpenAI credentials");
            result = OPENAI_OAUTH_TOKEN_TRANSIENT;
        }
    }
    else if (result == OPENAI_OAUTH_TOKEN_PERMANENT)
    {
        if (auth->session && session_manager_delete_provider_oauth(
                auth->session, OPENAI_PROVIDER_NAME) != 0)
        {
            log_error("delete permanently invalid OpenAI credentials", NULL);
            set_error_locked(auth, "Could not remove invalid OpenAI credentials");
            result = OPENAI_OAUTH_TOKEN_TRANSIENT;
        }
        else
        {
            clear_credentials_locked(auth);
            set_error_locked(auth, "OpenAI credentials are no longer valid");
        }
    }
    auth->last_refresh_result = result;
    auth->refresh_in_progress = 0;
    (void)pthread_cond_broadcast(&auth->condition);
    credentials_clear(&staged);
    if (result == OPENAI_OAUTH_TOKEN_OK &&
        copy_access_locked(auth, access_token, account_id) != 0)
        result = OPENAI_OAUTH_TOKEN_TRANSIENT;
    return result;
}

static OpenAIOAuthTokenResult get_access_token_internal(OpenAIOAuth *auth, int force,
                                                        char **access_token,
                                                        char **account_id)
{
    if (!auth || !access_token || !account_id) return OPENAI_OAUTH_TOKEN_SIGNED_OUT;
    *access_token = NULL;
    *account_id = NULL;
    if (pthread_mutex_lock(&auth->lock) != 0) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    if (!auth->session || !auth->access_token || !auth->refresh_token ||
        auth->destroying || auth->stop_requested)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return OPENAI_OAUTH_TOKEN_SIGNED_OUT;
    }
    OpenAIOAuthTokenResult result = refresh_credentials(auth, force,
                                                        access_token, account_id);
    if (pthread_mutex_unlock(&auth->lock) != 0)
     {
        secure_free(access_token);
        secure_free(account_id);
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    return result;
}

int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id)
{
    return openai_oauth_get_access_token_result(auth, access_token, account_id) ==
           OPENAI_OAUTH_TOKEN_OK ? 0 : -1;
}

OpenAIOAuthTokenResult openai_oauth_get_access_token_result(
    OpenAIOAuth *auth, char **access_token, char **account_id)
{
    return get_access_token_internal(auth, 0, access_token, account_id);
}

OpenAIOAuthTokenResult openai_oauth_force_refresh(OpenAIOAuth *auth,
                                                   char **access_token,
                                                   char **account_id)
{
    return get_access_token_internal(auth, 1, access_token, account_id);
}

OpenAIOAuthTokenResult openai_oauth_refresh_after_401(
    OpenAIOAuth *auth, const char *rejected_access_token,
    char **access_token, char **account_id)
{
    if (!auth || !rejected_access_token || !access_token || !account_id)
        return OPENAI_OAUTH_TOKEN_SIGNED_OUT;
    *access_token = NULL;
    *account_id = NULL;
    if (pthread_mutex_lock(&auth->lock) != 0) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    if (!auth->session || !auth->access_token || !auth->refresh_token ||
        auth->destroying || auth->stop_requested)
    {
        (void)pthread_mutex_unlock(&auth->lock);
        return OPENAI_OAUTH_TOKEN_SIGNED_OUT;
    }
    if (!state_matches(auth->access_token, rejected_access_token))
    {
        OpenAIOAuthTokenResult result = copy_access_locked(
            auth, access_token, account_id) == 0 ? OPENAI_OAUTH_TOKEN_OK :
                                                  OPENAI_OAUTH_TOKEN_TRANSIENT;
        (void)pthread_mutex_unlock(&auth->lock);
        return result;
    }
    OpenAIOAuthTokenResult result = refresh_credentials(
        auth, 1, access_token, account_id);
    if (pthread_mutex_unlock(&auth->lock) != 0)
    { secure_free(access_token); secure_free(account_id);
      return OPENAI_OAUTH_TOKEN_TRANSIENT; }
    return result;
}

int openai_oauth_logout(OpenAIOAuth *auth)
{
    if (!auth) return -1;
    pthread_t thread = {0};
    int joinable = 0;
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    if (auth->destroying || auth->lifecycle_busy)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    auth->lifecycle_busy = 1;
    cancel_callback_locked(auth);
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    if (join_callback_thread(thread, joinable) != 0)
     {
        lifecycle_finish(auth);
        return -1;
    }
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    while (auth->refresh_in_progress || auth->active_operations)
        if (pthread_cond_wait(&auth->condition, &auth->lock) != 0)
        { auth->lifecycle_busy = 0; (void)pthread_cond_broadcast(&auth->condition);
          (void)pthread_mutex_unlock(&auth->lock); return -1; }
    int result = auth->session ? session_manager_delete_provider_oauth(
        auth->session, OPENAI_PROVIDER_NAME) : -1;
    if (result == 0) clear_credentials_locked(auth);
    clear_pending_sensitive_locked(auth);
    secure_free(&auth->login_id);
    secure_free(&auth->last_error);
    auth->stop_requested = 0;
    auth->last_refresh_result = OPENAI_OAUTH_TOKEN_SIGNED_OUT;
    auth->lifecycle_busy = 0;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
    return result;
}

#ifdef OPENAI_OAUTH_TEST
char *openai_oauth_test_build_authorize_url_alloc(const char *state, const char *challenge)
{
    return build_authorize_url_values(state, challenge);
}

char *openai_oauth_test_pkce_challenge_alloc(const char *verifier)
{
    return pkce_challenge(verifier);
}

int openai_oauth_test_parse_callback(const void *request, size_t request_len,
                                     char **code, char **state, char **denial)
{
    if (!code || !state || !denial) return -1;
    *code = NULL;
    *state = NULL;
    *denial = NULL;
    OAuthCallback callback = {0};
    if (parse_callback_request(request, request_len, &callback) != 0) return -1;
    *code = callback.code;
    *state = callback.state;
    *denial = callback.denial;
    return 0;
}

int openai_oauth_test_parse_token(const char *json, int has_refresh,
                                  time_t now, char **access, char **refresh,
                                  char **account, char **plan, time_t *expires_at)
{
    if (!access || !refresh || !account || !plan || !expires_at) return -1;
    *access = NULL; *refresh = NULL; *account = NULL; *plan = NULL; *expires_at = 0;
    OAuthCredentials staged = {0};
    int result = token_set_parse(json, has_refresh ? "old-refresh" : NULL,
                                 NULL, NULL, now, &staged);
    if (result == 0)
    {
        *access = staged.access_token; staged.access_token = NULL;
        *refresh = staged.refresh_token; staged.refresh_token = NULL;
        *account = staged.account_id; staged.account_id = NULL;
        *plan = staged.plan_type; staged.plan_type = NULL;
        *expires_at = staged.expires_at;
    }
    credentials_clear(&staged);
    return result;
}

int openai_oauth_test_jwt_metadata(const char *jwt, char **account, char **plan)
{
    return jwt_metadata(jwt, account, plan);
}

int openai_oauth_test_needs_refresh(time_t expires_at, time_t now)
{
    return needs_refresh(expires_at, now);
}

int openai_oauth_test_parse_device_start(const char *json, char **device_auth_id,
                                          char **user_code,
                                          unsigned int *interval,
                                          unsigned int *expires_in)
{
    return parse_device_start(json, device_auth_id, user_code, interval, expires_in);
}

int openai_oauth_test_parse_device_authorization(const char *json, char **code,
                                                  char **verifier)
{
    return parse_device_authorization(json, code, verifier);
}
#endif
