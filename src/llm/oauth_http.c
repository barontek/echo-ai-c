/*
 * oauth_http.c - curl plumbing for the OpenAI OAuth token and device
 * endpoints: request body building, cancellable transfers, and
 * response classification.
 * Depends on: libcurl, cJSON, http_client, oauth_codec.
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include <openssl/crypto.h>

#include "oauth_http.h"
#include "oauth_codec.h"
#include "oauth_jwt.h"
#include "oauth_vault.h"
#include "openai_oauth_internal.h"
#include "../utils/http_client.h"
#include "../utils/logging.h"

static int curl_cancel_cb(void *userdata, curl_off_t download_total,
                          curl_off_t download_now, curl_off_t upload_total,
                          curl_off_t upload_now)
{
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    OAuthCancelContext *context = userdata;
    if (!context || pthread_mutex_lock(&context->auth->lock) != 0) return 1;
    int cancel = context->auth->destroying || context->auth->stop_requested ||
                 context->auth->generation != context->generation;
    if (pthread_mutex_unlock(&context->auth->lock) != 0) return 1;
    return cancel;
}

static int curl_set_common(CURL *curl, const char *url, const char *body,
                           HttpBuffer *buffer, OAuthCancelContext *cancel)
{
    if (!curl || !url || !body || !buffer || !cancel) return -1;
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_buffer_write_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_cancel_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel) != CURLE_OK)
        return -1;
    return 0;
}

static char *token_request_body(const char *grant_type, const char *value,
                                const char *redirect_uri, const char *verifier)
{
    char *grant = url_encode(grant_type);
    char *encoded_value = url_encode(value);
    char *client = url_encode(OPENAI_CLIENT_ID);
    char *body = NULL;
    if (grant && encoded_value && client && strcmp(grant_type, "refresh_token") == 0)
    {
        if (asprintf(&body, "grant_type=%s&refresh_token=%s&client_id=%s",
                     grant, encoded_value, client) < 0) body = NULL;
    }
    else if (grant && encoded_value && client && redirect_uri && verifier)
    {
        char *redirect = url_encode(redirect_uri);
        char *encoded_verifier = url_encode(verifier);
        if (redirect && encoded_verifier &&
            asprintf(&body, "grant_type=%s&code=%s&redirect_uri=%s&client_id=%s&"
                     "code_verifier=%s", grant, encoded_value, redirect, client,
                     encoded_verifier) < 0) body = NULL;
        free(redirect);
        secure_free(&encoded_verifier);
    }
    free(grant);
    secure_free(&encoded_value);
    free(client);
    return body;
}

static OpenAIOAuthTokenResult classify_exchange(CURLcode curl_result, long status,
                                                 int cancelled)
{
    if (cancelled || curl_result == CURLE_ABORTED_BY_CALLBACK)
        return OPENAI_OAUTH_TOKEN_CANCELLED;
    if (curl_result != CURLE_OK) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    if (status >= 200 && status < 300) return OPENAI_OAUTH_TOKEN_OK;
    if (status == 408 || status == 425 || status == 429 || status >= 500)
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    return OPENAI_OAUTH_TOKEN_PERMANENT;
}

OpenAIOAuthTokenResult exchange_token(OpenAIOAuth *auth, uint64_t generation,
                                              const char *grant_type, const char *value,
                                              const char *redirect_uri,
                                              const char *verifier, char **json_out)
{
    if (!auth || !json_out) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    *json_out = NULL;
    char *body = token_request_body(grant_type, value, redirect_uri, verifier);
    if (!body) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    CURL *curl = curl_easy_init();
    HttpBuffer buffer = {.limit = OAUTH_RESPONSE_MAX};
    OAuthCancelContext cancel = {.auth = auth, .generation = generation};
    struct curl_slist *headers = curl_slist_append(NULL,
        "Content-Type: application/x-www-form-urlencoded");
    CURLcode curl_result = CURLE_FAILED_INIT;
    long status = 0;
    if (curl && headers &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_set_common(curl, OPENAI_ISSUER "/oauth/token", body, &buffer, &cancel) == 0)
    {
        curl_result = curl_easy_perform(curl);
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
            curl_result = CURLE_HTTP_RETURNED_ERROR;
    }
    int cancelled = 0;
    if (pthread_mutex_lock(&auth->lock) == 0)
    {
        cancelled = auth->generation != generation || auth->stop_requested || auth->destroying;
        (void)pthread_mutex_unlock(&auth->lock);
    }
    OpenAIOAuthTokenResult result = classify_exchange(curl_result, status, cancelled);
    if (result == OPENAI_OAUTH_TOKEN_OK)
    {
        if (!buffer.data) result = OPENAI_OAUTH_TOKEN_TRANSIENT;
        else
        {
            *json_out = buffer.data;
            buffer.data = NULL;
        }
    }
    if (result != OPENAI_OAUTH_TOKEN_OK && result != OPENAI_OAUTH_TOKEN_CANCELLED)
        log_error("OpenAI OAuth token endpoint request failed", NULL);
    curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    if (buffer.data)
    {
        OPENSSL_cleanse(buffer.data, buffer.len);
        free(buffer.data);
    }
    OPENSSL_cleanse(body, strlen(body));
    free(body);
    return result;
}

OpenAIOAuthTokenResult device_post(OpenAIOAuth *auth, uint64_t generation,
                                          const char *path, const char *body,
                                          long *status_out, char **response_out)
{
    if (!auth || !path || !body || !status_out || !response_out)
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    *status_out = 0;
    *response_out = NULL;
    char *url = NULL;
    if (asprintf(&url, OPENAI_ISSUER "%s", path) < 0) return OPENAI_OAUTH_TOKEN_TRANSIENT;
    CURL *curl = curl_easy_init();
    HttpBuffer buffer = {.limit = OAUTH_RESPONSE_MAX};
    OAuthCancelContext cancel = {.auth = auth, .generation = generation};
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    CURLcode curl_result = CURLE_FAILED_INIT;
    if (curl && headers &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_set_common(curl, url, body, &buffer, &cancel) == 0)
    {
        curl_result = curl_easy_perform(curl);
        if (curl_result == CURLE_OK &&
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out) != CURLE_OK)
            curl_result = CURLE_HTTP_RETURNED_ERROR;
    }
    int cancelled = 0;
    if (pthread_mutex_lock(&auth->lock) == 0)
    {
        cancelled = auth->generation != generation || auth->stop_requested || auth->destroying;
        (void)pthread_mutex_unlock(&auth->lock);
    }
    OpenAIOAuthTokenResult result = cancelled || curl_result == CURLE_ABORTED_BY_CALLBACK ?
        OPENAI_OAUTH_TOKEN_CANCELLED :
        (curl_result == CURLE_OK ? OPENAI_OAUTH_TOKEN_OK : OPENAI_OAUTH_TOKEN_TRANSIENT);
    if (result == OPENAI_OAUTH_TOKEN_OK)
    {
        *response_out = buffer.data;
        buffer.data = NULL;
    }
    curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    if (buffer.data) {
        OPENSSL_cleanse(buffer.data, buffer.len);
        free(buffer.data);
    }
    free(url);
    return result;
}

char *device_json_body(const char *device_auth_id, const char *user_code)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    int valid = device_auth_id ||
                cJSON_AddStringToObject(json, "client_id", OPENAI_CLIENT_ID) != NULL;
    if (device_auth_id)
        valid = valid && cJSON_AddStringToObject(json, "device_auth_id",
                                                 device_auth_id) != NULL;
    if (user_code)
        valid = valid && cJSON_AddStringToObject(json, "user_code", user_code) != NULL;
    char *body = valid ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    return body;
}

static int json_uint_field(cJSON *json, const char *name, unsigned int maximum,
                           unsigned int *output)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!item || !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 1.0 || item->valuedouble > maximum ||
        floor(item->valuedouble) != item->valuedouble) return -1;
    *output = (unsigned int)item->valuedouble;
    return 0;
}

int parse_device_start(const char *response, char **device_auth_id,
                              char **user_code, unsigned int *interval,
                              unsigned int *expires_in)
{
    cJSON *json = NULL;
    if (!device_auth_id || !user_code || !interval || !expires_in ||
        exact_json_object(response, &json) != 0) return -1;
    *device_auth_id = NULL;
    *user_code = NULL;
    int valid = json_keys_are_unique(json) &&
                json_string_field(json, "device_auth_id", 1, device_auth_id) == 0 &&
                json_string_field(json, "user_code", 1, user_code) == 0 &&
                json_uint_field(json, "interval", 300, interval) == 0 &&
                json_uint_field(json, "expires_in", 3600, expires_in) == 0;
    cJSON_Delete(json);
    if (!valid)
    {
        secure_free(device_auth_id);
        secure_free(user_code);
        return -1;
    }
    return 0;
}
