/*
 * oauth_vault.c - credential staging, persistence, and secure clearing
 * for the OpenAI OAuth manager. All credential memory is cleansed
 * before free.
 * Depends on: cJSON, OpenSSL, SessionManager, oauth_jwt.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <cjson/cJSON.h>

#include "oauth_vault.h"
#include "oauth_jwt.h"
#include "oauth_codec.h"
#include "openai_oauth_internal.h"

void secure_free(char **value)
{
    if (!value || !*value) return;
    size_t len = strlen(*value);
    if (len > 0) OPENSSL_cleanse(*value, len);
    free(*value);
    *value = NULL;
}

uint64_t next_generation(uint64_t value)
{
    return value == UINT64_MAX ? 1 : value + 1;
}

void credentials_clear(OAuthCredentials *credentials)
{
    if (!credentials) return;
    secure_free(&credentials->access_token);
    secure_free(&credentials->refresh_token);
    secure_free(&credentials->account_id);
    secure_free(&credentials->plan_type);
    credentials->expires_at = 0;
}

void clear_credentials_locked(OpenAIOAuth *auth)
{
    secure_free(&auth->access_token);
    secure_free(&auth->refresh_token);
    secure_free(&auth->account_id);
    secure_free(&auth->plan_type);
    auth->expires_at = 0;
}

void clear_pending_sensitive_locked(OpenAIOAuth *auth)
{
    secure_free(&auth->state);
    secure_free(&auth->verifier);
    secure_free(&auth->challenge);
    secure_free(&auth->device_auth_id);
    secure_free(&auth->device_user_code);
    auth->device_expires_at = 0;
    auth->device_next_poll = 0;
    auth->device_interval = 0;
    auth->device_poll_in_progress = 0;
}

int token_set_parse(const char *data, const char *existing_refresh,
                           const char *existing_account, const char *existing_plan,
                           time_t now, OAuthCredentials *output)
{
    cJSON *json = NULL;
    if (!output || exact_json_object(data, &json) != 0) return -1;
    if (!json_keys_are_unique(json)) {
        cJSON_Delete(json);
        return -1;
    }
    OAuthCredentials staged = {0};
    int valid_fields = json_string_field(json, "access_token", 1,
                                         &staged.access_token) == 0 &&
                       json_string_field(json, "refresh_token", 0,
                                         &staged.refresh_token) == 0;
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires_in");
    double seconds = expires && cJSON_IsNumber(expires) ? expires->valuedouble : -1.0;
    int valid_expiry = isfinite(seconds) && seconds >= 1.0 &&
                       seconds <= 31536000.0 && floor(seconds) == seconds;
    if (!staged.refresh_token && existing_refresh)
        staged.refresh_token = string_dup(existing_refresh);
    char *id_token = NULL;
    if (json_string_field(json, "id_token", 0, &id_token) != 0) valid_fields = 0;
    int metadata_ok = 1;
    if (id_token)
        metadata_ok = jwt_metadata(id_token, &staged.account_id, &staged.plan_type) == 0;
    if (!staged.account_id && existing_account)
        staged.account_id = string_dup(existing_account);
    if (!staged.plan_type && existing_plan)
        staged.plan_type = string_dup(existing_plan);
    secure_free(&id_token);
    if (valid_expiry && now <= (time_t)(INT64_MAX - (int64_t)seconds))
        staged.expires_at = now + (time_t)seconds;
    cJSON_Delete(json);
    if (!valid_fields || !staged.access_token || !staged.refresh_token || !valid_expiry ||
        staged.expires_at <= now || !metadata_ok ||
        (existing_account && !staged.account_id) ||
        (existing_plan && !staged.plan_type))
     {
        credentials_clear(&staged);
        return -1;
    }
    *output = staged;
    return 0;
}

static char *credentials_json(const OAuthCredentials *credentials)
{
    if (!credentials || !credentials->access_token || !credentials->refresh_token)
        return NULL;
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    int valid = cJSON_AddStringToObject(json, "access_token", credentials->access_token) &&
                cJSON_AddStringToObject(json, "refresh_token", credentials->refresh_token) &&
                cJSON_AddNumberToObject(json, "expires_at", (double)credentials->expires_at);
    if (credentials->account_id)
        valid = valid && cJSON_AddStringToObject(json, "account_id", credentials->account_id);
    if (credentials->plan_type)
        valid = valid && cJSON_AddStringToObject(json, "plan_type", credentials->plan_type);
    char *result = valid ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    return result;
}

int stored_credentials_parse(const char *data, OAuthCredentials *output)
{
    cJSON *json = NULL;
    if (!output || exact_json_object(data, &json) != 0) return -1;
    if (!json_keys_are_unique(json)) {
        cJSON_Delete(json);
        return -1;
    }
    OAuthCredentials staged = {0};
    int valid_fields = json_string_field(json, "access_token", 1,
                                         &staged.access_token) == 0 &&
                       json_string_field(json, "refresh_token", 1,
                                         &staged.refresh_token) == 0 &&
                       json_string_field(json, "account_id", 0,
                                         &staged.account_id) == 0 &&
                       json_string_field(json, "plan_type", 0,
                                         &staged.plan_type) == 0;
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires_at");
    double value = expires && cJSON_IsNumber(expires) ? expires->valuedouble : -1.0;
    int valid = valid_fields && staged.access_token && staged.refresh_token && isfinite(value) &&
                value >= 0.0 && value <= (double)INT64_MAX && floor(value) == value;
    if (valid) staged.expires_at = (time_t)value;
    cJSON_Delete(json);
    if (!valid) {
        credentials_clear(&staged);
        return -1;
    }
    *output = staged;
    return 0;
}

void commit_credentials_locked(OpenAIOAuth *auth, OAuthCredentials *staged)
{
    clear_credentials_locked(auth);
    auth->access_token = staged->access_token;
    auth->refresh_token = staged->refresh_token;
    auth->account_id = staged->account_id;
    auth->plan_type = staged->plan_type;
    auth->expires_at = staged->expires_at;
    memset(staged, 0, sizeof(*staged));
    auth->token_version = next_generation(auth->token_version);
}

int persist_staged_locked(OpenAIOAuth *auth,
                                 const OAuthCredentials *credentials)
{
    if (!auth->session) return -1;
    char *data = credentials_json(credentials);
    if (!data) return -1;
    int result = session_manager_save_provider_oauth(auth->session,
                                                      OPENAI_PROVIDER_NAME, data);
    OPENSSL_cleanse(data, strlen(data));
    free(data);
    return result;
}
