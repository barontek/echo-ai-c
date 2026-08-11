/*
 * oauth_device.c - device-flow login state machine for the OpenAI
 * OAuth manager: device start, polling, and authorization exchange.
 * Depends on: oauth_http, oauth_callback, oauth_vault, oauth_jwt.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <cjson/cJSON.h>

#include "oauth_device.h"
#include "oauth_http.h"
#include "oauth_callback.h"
#include "oauth_vault.h"
#include "oauth_jwt.h"
#include "oauth_codec.h"
#include "openai_oauth_internal.h"

static void device_start_failed(OpenAIOAuth *auth, uint64_t generation,
                                const char *message)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->generation == generation)
    {
        clear_pending_sensitive_locked(auth);
        secure_free(&auth->login_id);
        auth->callback_active = 0;
        set_error_locked(auth, message);
        (void)pthread_cond_broadcast(&auth->condition);
    }
    (void)pthread_mutex_unlock(&auth->lock);
}

int openai_oauth_device_start(OpenAIOAuth *auth, char **verification_url,
                              char **user_code, char **login_id,
                              unsigned int *poll_interval_seconds)
{
    if (!auth || !verification_url || !user_code || !login_id ||
        !poll_interval_seconds) return -1;
    *verification_url = NULL; *user_code = NULL; *login_id = NULL;
    *poll_interval_seconds = 0;
    if (reap_previous_callback(auth) != 0) return -1;
    char *id = NULL;
    char *body = device_json_body(NULL, NULL);
    if (random_string(&id) != 0 || !body) {
        secure_free(&id);
        free(body);
        return -1;
    }
    if (pthread_mutex_lock(&auth->lock) != 0)
     {
        secure_free(&id);
        OPENSSL_cleanse(body, strlen(body));
        free(body);
        return -1;
    }
    if (!auth->session || auth->callback_active || auth->destroying)
    {
        (void)pthread_mutex_unlock(&auth->lock);
        secure_free(&id); OPENSSL_cleanse(body, strlen(body)); free(body);
        return -1;
    }
    clear_pending_sensitive_locked(auth);
    secure_free(&auth->login_id);
    auth->login_id = id; id = NULL;
    secure_free(&auth->last_error);
    auth->generation = next_generation(auth->generation);
    auth->stop_requested = 0;
    auth->callback_active = 1;
    auth->active_operations++;
    uint64_t generation = auth->generation;
    (void)pthread_mutex_unlock(&auth->lock);

    long status = 0;
    char *response = NULL;
    OpenAIOAuthTokenResult request = device_post(auth, generation,
        "/api/accounts/deviceauth/usercode", body, &status, &response);
    OPENSSL_cleanse(body, strlen(body));
    free(body);
    char *device_auth_id = NULL;
    char *code = NULL;
    unsigned int interval = 0;
    unsigned int expires_in = 0;
    time_t now = time(NULL);
    int valid = request == OPENAI_OAUTH_TOKEN_OK && status >= 200 && status < 300 &&
                response && now != (time_t)-1 &&
                parse_device_start(response, &device_auth_id, &code,
                                   &interval, &expires_in) == 0;
    if (response) {
        OPENSSL_cleanse(response, strlen(response));
        free(response);
    }
    char *url_copy = valid ? string_dup(OPENAI_DEVICE_VERIFICATION_URL) : NULL;
    char *code_copy = valid ? string_dup(code) : NULL;
    char *id_copy = NULL;
    if (valid && pthread_mutex_lock(&auth->lock) == 0)
    {
        if (auth->generation == generation && !auth->stop_requested &&
            url_copy && code_copy)
        {
            id_copy = string_dup(auth->login_id);
            if (id_copy)
            {
                auth->device_auth_id = device_auth_id; device_auth_id = NULL;
                auth->device_user_code = code; code = NULL;
                auth->device_interval = interval;
                auth->device_next_poll = now + (time_t)interval;
                auth->device_expires_at = now + (time_t)expires_in;
            }
        }
        (void)pthread_mutex_unlock(&auth->lock);
    }
    if (!id_copy)
    {
        device_start_failed(auth, generation, request == OPENAI_OAUTH_TOKEN_CANCELLED ?
            "OpenAI device login was cancelled" : "Could not start OpenAI device login");
        free(url_copy); secure_free(&code_copy);
    }
    else
    {
        *verification_url = url_copy;
        *user_code = code_copy;
        *login_id = id_copy;
        *poll_interval_seconds = interval;
    }
    secure_free(&device_auth_id);
    secure_free(&code);
    active_operation_finish(auth);
    return id_copy ? 0 : -1;
}

static OpenAIOAuthDeviceResult device_error_result(const char *response,
                                                    long status, int *slow_down)
{
    *slow_down = 0;
    cJSON *json = NULL;
    char *error = NULL;
    if (response && exact_json_object(response, &json) == 0 &&
        json_keys_are_unique(json) &&
        json_string_field(json, "error", 0, &error) == 0 && error)
    {
        if (strcmp(error, "authorization_pending") == 0)
         {
            secure_free(&error);
            cJSON_Delete(json);
            return OPENAI_OAUTH_DEVICE_PENDING;
        }
        if (strcmp(error, "slow_down") == 0)
        {
            *slow_down = 1;
            secure_free(&error);
            cJSON_Delete(json);
            return OPENAI_OAUTH_DEVICE_PENDING;
        }
    }
    secure_free(&error);
    cJSON_Delete(json);
    return status == 408 || status == 425 || status == 429 || status >= 500 ?
        OPENAI_OAUTH_DEVICE_TRANSIENT : OPENAI_OAUTH_DEVICE_TERMINAL;
}

int parse_device_authorization(const char *response, char **code,
                                      char **verifier)
{
    cJSON *json = NULL;
    if (!code || !verifier || exact_json_object(response, &json) != 0) return -1;
    *code = NULL;
    *verifier = NULL;
    int valid = json_keys_are_unique(json) &&
                json_string_field(json, "authorization_code", 1, code) == 0 &&
                json_string_field(json, "code_verifier", 1, verifier) == 0;
    cJSON_Delete(json);
    if (!valid) {
        secure_free(code);
        secure_free(verifier);
        return -1;
    }
    return 0;
}

static void device_poll_finished(OpenAIOAuth *auth, uint64_t generation,
                                 OpenAIOAuthDeviceResult result)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->generation == generation)
    {
        auth->callback_active = 0;
        clear_pending_sensitive_locked(auth);
        if (result == OPENAI_OAUTH_DEVICE_TERMINAL)
            set_error_locked(auth, "OpenAI device login was rejected or expired");
        else if (result != OPENAI_OAUTH_DEVICE_COMPLETE)
            set_error_locked(auth, "OpenAI device login failed");
        (void)pthread_cond_broadcast(&auth->condition);
    }
    (void)pthread_mutex_unlock(&auth->lock);
}

static void device_poll_released(OpenAIOAuth *auth, uint64_t generation)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->generation == generation) auth->device_poll_in_progress = 0;
    if (auth->active_operations > 0) auth->active_operations--;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
}

OpenAIOAuthDeviceResult openai_oauth_device_poll(OpenAIOAuth *auth,
                                                 const char *login_id)
{
    if (!auth || !login_id || pthread_mutex_lock(&auth->lock) != 0)
        return OPENAI_OAUTH_DEVICE_TERMINAL;
    time_t now = time(NULL);
    if (!auth->callback_active || !auth->device_auth_id || !auth->device_user_code ||
        !auth->login_id || !state_matches(auth->login_id, login_id) ||
        now == (time_t)-1)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return OPENAI_OAUTH_DEVICE_TERMINAL;
    }
    if (now >= auth->device_expires_at)
    {
        uint64_t generation = auth->generation;
        (void)pthread_mutex_unlock(&auth->lock);
        device_poll_finished(auth, generation, OPENAI_OAUTH_DEVICE_TERMINAL);
        return OPENAI_OAUTH_DEVICE_TERMINAL;
    }
    if (auth->device_poll_in_progress)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return OPENAI_OAUTH_DEVICE_PENDING;
    }
    if (now < auth->device_next_poll)
     {
        (void)pthread_mutex_unlock(&auth->lock);
        return OPENAI_OAUTH_DEVICE_PENDING;
    }
    char *device_auth_id = string_dup(auth->device_auth_id);
    char *user_code = string_dup(auth->device_user_code);
    uint64_t generation = auth->generation;
    auth->device_next_poll = now + (time_t)auth->device_interval;
    auth->device_poll_in_progress = 1;
    auth->active_operations++;
    (void)pthread_mutex_unlock(&auth->lock);
    if (!device_auth_id || !user_code)
    { secure_free(&device_auth_id); secure_free(&user_code);
      device_poll_released(auth, generation);
      return OPENAI_OAUTH_DEVICE_TRANSIENT; }
    char *body = device_json_body(device_auth_id, user_code);
    secure_free(&device_auth_id);
    secure_free(&user_code);
    if (!body)
     {
        device_poll_released(auth, generation);
        return OPENAI_OAUTH_DEVICE_TRANSIENT;
    }
    long status = 0;
    char *response = NULL;
    OpenAIOAuthTokenResult request = device_post(auth, generation,
        "/api/accounts/deviceauth/token", body, &status, &response);
    OPENSSL_cleanse(body, strlen(body));
    free(body);
    if (request == OPENAI_OAUTH_TOKEN_CANCELLED)
    {
        if (response)
        {
            OPENSSL_cleanse(response, strlen(response));
            free(response);
        }
        device_poll_released(auth, generation);
        return OPENAI_OAUTH_DEVICE_CANCELLED;
    }
    if (request != OPENAI_OAUTH_TOKEN_OK)
    {
        if (response)
        {
            OPENSSL_cleanse(response, strlen(response));
            free(response);
        }
        device_poll_released(auth, generation);
        return OPENAI_OAUTH_DEVICE_TRANSIENT;
    }
    int slow_down = 0;
    OpenAIOAuthDeviceResult result = status >= 200 && status < 300 ?
        OPENAI_OAUTH_DEVICE_COMPLETE : device_error_result(response, status, &slow_down);
    if (result == OPENAI_OAUTH_DEVICE_PENDING)
    {
        if (slow_down && pthread_mutex_lock(&auth->lock) == 0)
        {
            if (auth->generation == generation && auth->device_interval <= UINT_MAX - 5)
            {
                auth->device_interval += 5;
                time_t delayed = time(NULL);
                if (delayed != (time_t)-1)
                    auth->device_next_poll = delayed + (time_t)auth->device_interval;
            }
            (void)pthread_mutex_unlock(&auth->lock);
        }
        if (response) {
            OPENSSL_cleanse(response, strlen(response));
            free(response);
        }
        device_poll_released(auth, generation);
        return result;
    }
    char *authorization_code = NULL;
    char *verifier = NULL;
    if (result == OPENAI_OAUTH_DEVICE_COMPLETE &&
        parse_device_authorization(response, &authorization_code, &verifier) != 0)
        result = OPENAI_OAUTH_DEVICE_TRANSIENT;
    if (response) {
        OPENSSL_cleanse(response, strlen(response));
        free(response);
    }
    if (result == OPENAI_OAUTH_DEVICE_COMPLETE)
    {
        char *tokens = NULL;
        OpenAIOAuthTokenResult exchange = exchange_token(auth, generation,
            "authorization_code", authorization_code, OPENAI_DEVICE_REDIRECT_URI,
            verifier, &tokens);
        if (exchange == OPENAI_OAUTH_TOKEN_OK &&
            complete_callback_tokens(auth, generation, tokens) == 0)
            result = OPENAI_OAUTH_DEVICE_COMPLETE;
        else result = exchange == OPENAI_OAUTH_TOKEN_CANCELLED ?
            OPENAI_OAUTH_DEVICE_CANCELLED : OPENAI_OAUTH_DEVICE_TRANSIENT;
        if (tokens) {
            OPENSSL_cleanse(tokens, strlen(tokens));
            free(tokens);
        }
    }
    secure_free(&authorization_code);
    secure_free(&verifier);
    if (result != OPENAI_OAUTH_DEVICE_CANCELLED)
        device_poll_finished(auth, generation, result);
    active_operation_finish(auth);
    return result;
}
