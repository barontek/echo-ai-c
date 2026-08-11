/*
 * oauth_jwt.c - id_token parsing and JSON field helpers for the OpenAI
 * OAuth flows: JWT payload decoding, duplicate-key rejection, refresh
 * window arithmetic.
 * Depends on: cJSON, OpenSSL, oauth_codec.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <cjson/cJSON.h>

#include "oauth_jwt.h"
#include "oauth_codec.h"
#include "oauth_vault.h"
#include "openai_oauth_internal.h"

int json_string_field(cJSON *object, const char *name, int required,
                             char **output)
{
    if (!object || !name || !output) return -1;
    *output = NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) return required ? -1 : 0;
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) return -1;
    size_t len = strlen(item->valuestring);
    if (len > OAUTH_VALUE_MAX) return -1;
    for (size_t index = 0; index < len; index++)
        if ((unsigned char)item->valuestring[index] < 0x20) return -1;
    *output = string_dup(item->valuestring);
    return *output ? 0 : -1;
}

static cJSON *jwt_payload_json(const char *jwt)
{
    if (!jwt || strlen(jwt) > OAUTH_VALUE_MAX) return NULL;
    const char *first = strchr(jwt, '.');
    if (!first || first == jwt) return NULL;
    const char *second = strchr(first + 1, '.');
    if (!second || second == first + 1 || second[1] == '\0' || strchr(second + 1, '.'))
        return NULL;
    size_t encoded_len = (size_t)(second - first - 1);
    char *encoded = strndup(first + 1, encoded_len);
    if (!encoded) return NULL;
    size_t decoded_len = 0;
    unsigned char *decoded = base64url_decode(encoded, &decoded_len);
    free(encoded);
    if (!decoded || memchr(decoded, '\0', decoded_len))
    {
        if (decoded) OPENSSL_cleanse(decoded, decoded_len);
        free(decoded);
        return NULL;
    }
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts((const char *)decoded, &end, 1);
    int parsed_exactly = end && *end == '\0';
    OPENSSL_cleanse(decoded, decoded_len);
    free(decoded);
    if (!json || !cJSON_IsObject(json) || !parsed_exactly ||
        !json_keys_are_unique(json))
     {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

int jwt_metadata(const char *jwt, char **account, char **plan)
{
    if (!account || !plan) return -1;
    *account = NULL;
    *plan = NULL;
    cJSON *payload = jwt_payload_json(jwt);
    if (!payload) return -1;
    if (json_string_field(payload, "chatgpt_account_id", 0, account) != 0)
     {
        cJSON_Delete(payload);
        return -1;
    }
    cJSON *claims = cJSON_GetObjectItemCaseSensitive(payload,
                                                     "https://api.openai.com/auth");
    if (claims && !cJSON_IsObject(claims))
     {
        secure_free(account);
        cJSON_Delete(payload);
        return -1;
    }
    if (claims && !json_keys_are_unique(claims))
     {
        secure_free(account);
        cJSON_Delete(payload);
        return -1;
    }
    if (claims)
    {
        char *nested_account = NULL;
        if (json_string_field(claims, "chatgpt_account_id", 0, &nested_account) != 0 ||
            json_string_field(claims, "chatgpt_plan_type", 0, plan) != 0)
        {
            secure_free(&nested_account);
            secure_free(account);
            secure_free(plan);
            cJSON_Delete(payload);
            return -1;
        }
        if (!*account) {
            *account = nested_account;
            nested_account = NULL;
        }
        secure_free(&nested_account);
    }
    cJSON_Delete(payload);
    return 0;
}

int exact_json_object(const char *data, cJSON **output)
{
    if (!data || !output || strlen(data) > OAUTH_RESPONSE_MAX) return -1;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts(data, &end, 1);
    if (!json || !cJSON_IsObject(json) || !end || *end != '\0')
     {
        cJSON_Delete(json);
        return -1;
    }
    *output = json;
    return 0;
}

int json_keys_are_unique(cJSON *object)
{
    if (!object || !cJSON_IsObject(object)) return 0;
    for (cJSON *item = object->child; item; item = item->next)
    {
        if (!item->string) return 0;
        for (cJSON *other = item->next; other; other = other->next)
            if (other->string && strcmp(item->string, other->string) == 0) return 0;
    }
    return 1;
}

int needs_refresh(time_t expires_at, time_t now)
{
    if (expires_at <= now) return 1;
    return expires_at - now <= OAUTH_REFRESH_SKEW_SECONDS;
}
