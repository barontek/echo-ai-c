/*
 * openai_oauth.c - OAuth manager for the ChatGPT Codex provider: device
 * and localhost-callback login flows, PKCE, single-flight token refresh,
 * and encrypted credential persistence via SessionManager.
 * Depends on: libcurl, cJSON, OpenSSL, pthread, SessionManager, logging.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "openai_oauth.h"
#include "../utils/http_client.h"
#include "../utils/logging.h"

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
#define OAUTH_RESPONSE_MAX (1024U * 1024U)
#define OAUTH_VALUE_MAX (256U * 1024U)
#define OAUTH_QUERY_FIELDS_MAX 16

typedef struct {
    char *access_token;
    char *refresh_token;
    char *account_id;
    char *plan_type;
    time_t expires_at;
} OAuthCredentials;

typedef struct {
    char *code;
    char *state;
    char *denial;
} OAuthCallback;

typedef struct {
    struct OpenAIOAuth *auth;
    uint64_t generation;
} OAuthThreadArgs;

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

static void secure_free(char **value)
{
    if (!value || !*value) return;
    size_t len = strlen(*value);
    if (len > 0) OPENSSL_cleanse(*value, len);
    free(*value);
    *value = NULL;
}

static char *string_dup(const char *value)
{
    if (!value) return NULL;
    size_t len = strlen(value);
    if (len == SIZE_MAX) return NULL;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static void credentials_clear(OAuthCredentials *credentials)
{
    if (!credentials) return;
    secure_free(&credentials->access_token);
    secure_free(&credentials->refresh_token);
    secure_free(&credentials->account_id);
    secure_free(&credentials->plan_type);
    credentials->expires_at = 0;
}

static void clear_credentials_locked(OpenAIOAuth *auth)
{
    secure_free(&auth->access_token);
    secure_free(&auth->refresh_token);
    secure_free(&auth->account_id);
    secure_free(&auth->plan_type);
    auth->expires_at = 0;
}

static void clear_pending_sensitive_locked(OpenAIOAuth *auth)
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

static void set_error_locked(OpenAIOAuth *auth, const char *message)
{
    secure_free(&auth->last_error);
    auth->last_error = string_dup(message ? message : "OpenAI OAuth failed");
}

static uint64_t next_generation(uint64_t value)
{
    return value == UINT64_MAX ? 1 : value + 1;
}

static int is_url_char(unsigned char value)
{
    return isalnum(value) || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

static char *url_encode(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!value) return NULL;
    size_t len = strlen(value);
    if (len > (SIZE_MAX - 1) / 3) return NULL;
    char *encoded = malloc(len * 3 + 1);
    if (!encoded) return NULL;
    size_t output = 0;
    for (size_t index = 0; index < len; index++)
    {
        unsigned char byte = (unsigned char)value[index];
        if (is_url_char(byte)) encoded[output++] = (char)byte;
        else
        {
            encoded[output++] = '%';
            encoded[output++] = hex[byte >> 4];
            encoded[output++] = hex[byte & 0x0f];
        }
    }
    encoded[output] = '\0';
    return encoded;
}

static char *base64url_encode(const unsigned char *data, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (!data || len > (SIZE_MAX - 2) / 4 * 3) return NULL;
    size_t output_len = ((len + 2) / 3) * 4;
    char *output = malloc(output_len + 1);
    if (!output) return NULL;
    size_t output_index = 0;
    for (size_t index = 0; index < len; index += 3)
    {
        size_t remaining = len - index;
        unsigned int value = (unsigned int)data[index] << 16;
        if (remaining > 1) value |= (unsigned int)data[index + 1] << 8;
        if (remaining > 2) value |= data[index + 2];
        output[output_index++] = table[(value >> 18) & 63U];
        output[output_index++] = table[(value >> 12) & 63U];
        if (remaining > 1) output[output_index++] = table[(value >> 6) & 63U];
        if (remaining > 2) output[output_index++] = table[value & 63U];
    }
    output[output_index] = '\0';
    return output;
}

static char *pkce_challenge(const char *verifier)
{
    if (!verifier) return NULL;
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    if (!SHA256((const unsigned char *)verifier, strlen(verifier), digest)) return NULL;
    char *challenge = base64url_encode(digest, sizeof(digest));
    OPENSSL_cleanse(digest, sizeof(digest));
    return challenge;
}

static int random_string(char **output)
{
    unsigned char bytes[32] = {0};
    if (!output) return -1;
    *output = NULL;
    if (RAND_bytes(bytes, (int)sizeof(bytes)) != 1) return -1;
    *output = base64url_encode(bytes, sizeof(bytes));
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return *output ? 0 : -1;
}

static int make_pkce(char **verifier, char **challenge)
{
    if (!verifier || !challenge) return -1;
    *verifier = NULL;
    *challenge = NULL;
    if (random_string(verifier) != 0) return -1;
    *challenge = pkce_challenge(*verifier);
    if (!*challenge) { secure_free(verifier); return -1; }
    return 0;
}

static char *build_authorize_url_values(const char *state, const char *challenge)
{
    char *redirect = url_encode(OPENAI_REDIRECT_URI);
    char *scope = url_encode("openid profile email offline_access");
    char *originator = url_encode("echo-ai");
    char *encoded_state = url_encode(state);
    char *encoded_challenge = url_encode(challenge);
    char *url = NULL;
    if (redirect && scope && originator && encoded_state && encoded_challenge &&
        asprintf(&url, OPENAI_ISSUER "/oauth/authorize?response_type=code&client_id=%s&"
                 "redirect_uri=%s&scope=%s&code_challenge=%s&code_challenge_method=S256&"
                 "id_token_add_organizations=true&codex_cli_simplified_flow=true&"
                 "originator=%s&state=%s", OPENAI_CLIENT_ID, redirect, scope,
                 encoded_challenge, originator, encoded_state) < 0)
        url = NULL;
    free(redirect);
    free(scope);
    free(originator);
    free(encoded_state);
    free(encoded_challenge);
    return url;
}

static int hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static char *url_decode_exact(const unsigned char *value, size_t len)
{
    if (!value || len == 0 || len == SIZE_MAX) return NULL;
    char *decoded = malloc(len + 1);
    if (!decoded) return NULL;
    size_t output = 0;
    for (size_t index = 0; index < len; index++)
    {
        unsigned char byte = value[index];
        if (byte == '%')
        {
            if (index + 2 >= len) { free(decoded); return NULL; }
            int high = hex_value(value[index + 1]);
            int low = hex_value(value[index + 2]);
            if (high < 0 || low < 0) { free(decoded); return NULL; }
            byte = (unsigned char)((high << 4) | low);
            index += 2;
        }
        else if (byte == '+') byte = ' ';
        if (byte == 0 || byte < 0x20 || byte == 0x7f)
        { free(decoded); return NULL; }
        decoded[output++] = (char)byte;
    }
    decoded[output] = '\0';
    return decoded;
}

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
    while (len > 0 && (*value == ' ' || *value == '\t')) { value++; len--; }
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
        if (!name || !value) { free(name); secure_free(&value); goto cleanup; }
        names[count] = name;
        if (callback_set_field(callback, names, count, name, value) != 0)
        { free(name); names[count] = NULL; secure_free(&value); goto cleanup; }
        count++;
        if (!amp) { offset = len; break; }
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

static int parse_callback_request(const void *request, size_t request_len,
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

static int base64url_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '-') return 62;
    if (value == '_') return 63;
    return -1;
}

static unsigned char *base64url_decode(const char *input, size_t *output_len)
{
    if (!input || !output_len) return NULL;
    size_t len = strlen(input);
    if (len == 0 || len % 4 == 1 || len > OAUTH_VALUE_MAX) return NULL;
    size_t decoded_len = len / 4 * 3;
    if (len % 4 == 2) decoded_len++;
    if (len % 4 == 3) decoded_len += 2;
    unsigned char *output = malloc(decoded_len + 1);
    if (!output) return NULL;
    size_t output_index = 0;
    for (size_t index = 0; index < len; index += 4)
    {
        size_t remaining = len - index;
        int a = base64url_value((unsigned char)input[index]);
        int b = remaining > 1 ? base64url_value((unsigned char)input[index + 1]) : -1;
        int c = remaining > 2 ? base64url_value((unsigned char)input[index + 2]) : 0;
        int d = remaining > 3 ? base64url_value((unsigned char)input[index + 3]) : 0;
        if (a < 0 || b < 0 || (remaining > 2 && c < 0) || (remaining > 3 && d < 0))
        { free(output); return NULL; }
        unsigned int value = ((unsigned int)a << 18) | ((unsigned int)b << 12) |
                             ((unsigned int)c << 6) | (unsigned int)d;
        output[output_index++] = (unsigned char)(value >> 16);
        if (remaining > 2) output[output_index++] = (unsigned char)(value >> 8);
        if (remaining > 3) output[output_index++] = (unsigned char)value;
    }
    output[output_index] = '\0';
    *output_len = output_index;
    return output;
}

static int json_string_field(cJSON *object, const char *name, int required,
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

static int json_keys_are_unique(cJSON *object);

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
    { cJSON_Delete(json); return NULL; }
    return json;
}

static int jwt_metadata(const char *jwt, char **account, char **plan)
{
    if (!account || !plan) return -1;
    *account = NULL;
    *plan = NULL;
    cJSON *payload = jwt_payload_json(jwt);
    if (!payload) return -1;
    if (json_string_field(payload, "chatgpt_account_id", 0, account) != 0)
    { cJSON_Delete(payload); return -1; }
    cJSON *claims = cJSON_GetObjectItemCaseSensitive(payload,
                                                     "https://api.openai.com/auth");
    if (claims && !cJSON_IsObject(claims))
    { secure_free(account); cJSON_Delete(payload); return -1; }
    if (claims && !json_keys_are_unique(claims))
    { secure_free(account); cJSON_Delete(payload); return -1; }
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
        if (!*account) { *account = nested_account; nested_account = NULL; }
        secure_free(&nested_account);
    }
    cJSON_Delete(payload);
    return 0;
}

static int exact_json_object(const char *data, cJSON **output)
{
    if (!data || !output || strlen(data) > OAUTH_RESPONSE_MAX) return -1;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts(data, &end, 1);
    if (!json || !cJSON_IsObject(json) || !end || *end != '\0')
    { cJSON_Delete(json); return -1; }
    *output = json;
    return 0;
}

static int json_keys_are_unique(cJSON *object)
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

static int token_set_parse(const char *data, const char *existing_refresh,
                           const char *existing_account, const char *existing_plan,
                           time_t now, OAuthCredentials *output)
{
    cJSON *json = NULL;
    if (!output || exact_json_object(data, &json) != 0) return -1;
    if (!json_keys_are_unique(json)) { cJSON_Delete(json); return -1; }
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
    { credentials_clear(&staged); return -1; }
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

static int stored_credentials_parse(const char *data, OAuthCredentials *output)
{
    cJSON *json = NULL;
    if (!output || exact_json_object(data, &json) != 0) return -1;
    if (!json_keys_are_unique(json)) { cJSON_Delete(json); return -1; }
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
    if (!valid) { credentials_clear(&staged); return -1; }
    *output = staged;
    return 0;
}

static void commit_credentials_locked(OpenAIOAuth *auth, OAuthCredentials *staged)
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

static int persist_staged_locked(OpenAIOAuth *auth,
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

static OpenAIOAuthTokenResult exchange_token(OpenAIOAuth *auth, uint64_t generation,
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

static OpenAIOAuthTokenResult device_post(OpenAIOAuth *auth, uint64_t generation,
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
    if (buffer.data) { OPENSSL_cleanse(buffer.data, buffer.len); free(buffer.data); }
    free(url);
    return result;
}

static char *device_json_body(const char *device_auth_id, const char *user_code)
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

static int parse_device_start(const char *response, char **device_auth_id,
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
        if (end) { *request_len = used; return 0; }
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

static int state_matches(const char *expected, const char *actual)
{
    if (!expected || !actual) return 0;
    size_t expected_len = strlen(expected);
    size_t actual_len = strlen(actual);
    return expected_len == actual_len &&
           CRYPTO_memcmp(expected, actual, expected_len) == 0;
}

static int complete_callback_tokens(OpenAIOAuth *auth, uint64_t generation,
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
        if (json) { OPENSSL_cleanse(json, strlen(json)); free(json); }
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
        { *timed_out = 1; return -1; }
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

static void *callback_thread_main(void *userdata)
{
    OAuthThreadArgs *args = userdata;
    OpenAIOAuth *auth = args->auth;
    uint64_t generation = args->generation;
    free(args);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int timed_out = 0;
    if (listener < 0) { callback_publish_failure(auth, generation); goto finished; }
    int reuse = 1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(OPENAI_CALLBACK_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0 ||
        callback_publish_listener(auth, generation, listener) != 0)
    { callback_publish_failure(auth, generation); goto close_listener; }
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

static void shutdown_fd(int fd)
{
    if (fd >= 0 && shutdown(fd, SHUT_RDWR) != 0 && errno != ENOTCONN && errno != EINVAL)
        log_error("shutdown OpenAI OAuth socket", NULL);
}

static void cancel_callback_locked(OpenAIOAuth *auth)
{
    auth->generation = next_generation(auth->generation);
    auth->stop_requested = 1;
    shutdown_fd(auth->client_fd);
    shutdown_fd(auth->listener_fd);
    clear_pending_sensitive_locked(auth);
    auth->callback_active = 0;
}

static int take_callback_thread_locked(OpenAIOAuth *auth, pthread_t *thread)
{
    if (!auth->thread_joinable) return 0;
    *thread = auth->callback_thread;
    auth->thread_joinable = 0;
    return 1;
}

static int join_callback_thread(pthread_t thread, int joinable)
{
    return !joinable || pthread_join(thread, NULL) == 0 ? 0 : -1;
}

static void lifecycle_finish(OpenAIOAuth *auth)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    auth->lifecycle_busy = 0;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
}

static void active_operation_finish(OpenAIOAuth *auth)
{
    if (pthread_mutex_lock(&auth->lock) != 0) return;
    if (auth->active_operations > 0) auth->active_operations--;
    (void)pthread_cond_broadcast(&auth->condition);
    (void)pthread_mutex_unlock(&auth->lock);
}

OpenAIOAuth *openai_oauth_create(void)
{
    OpenAIOAuth *auth = calloc(1, sizeof(*auth));
    if (!auth) return NULL;
    if (pthread_mutex_init(&auth->lock, NULL) != 0)
    { free(auth); return NULL; }
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
    { (void)pthread_mutex_unlock(&auth->lock); return -1; }
    auth->lifecycle_busy = 1;
    cancel_callback_locked(auth);
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    if (join_callback_thread(thread, joinable) != 0)
    { lifecycle_finish(auth); return -1; }
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
    if (data) { OPENSSL_cleanse(data, strlen(data)); free(data); }
    if (pthread_mutex_lock(&auth->lock) != 0)
    { credentials_clear(&staged); return -1; }
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

static int reap_previous_callback(OpenAIOAuth *auth)
{
    pthread_t thread = {0};
    int joinable = 0;
    if (pthread_mutex_lock(&auth->lock) != 0) return -1;
    if (auth->callback_active || auth->destroying || auth->lifecycle_busy)
    { (void)pthread_mutex_unlock(&auth->lock); return -1; }
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    return join_callback_thread(thread, joinable);
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
    if (!url || !id_copy || !args) { free(url); free(id_copy); free(args); goto cleanup; }
    if (pthread_mutex_lock(&auth->lock) != 0)
    { free(url); free(id_copy); free(args); goto cleanup; }
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
        { auth->callback_rc = -1; break; }
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
    if (random_string(&id) != 0 || !body) { secure_free(&id); free(body); return -1; }
    if (pthread_mutex_lock(&auth->lock) != 0)
    { secure_free(&id); OPENSSL_cleanse(body, strlen(body)); free(body); return -1; }
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
    if (response) { OPENSSL_cleanse(response, strlen(response)); free(response); }
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
        { secure_free(&error); cJSON_Delete(json); return OPENAI_OAUTH_DEVICE_PENDING; }
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

static int parse_device_authorization(const char *response, char **code,
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
    if (!valid) { secure_free(code); secure_free(verifier); return -1; }
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
    { (void)pthread_mutex_unlock(&auth->lock); return OPENAI_OAUTH_DEVICE_TERMINAL; }
    if (now >= auth->device_expires_at)
    {
        uint64_t generation = auth->generation;
        (void)pthread_mutex_unlock(&auth->lock);
        device_poll_finished(auth, generation, OPENAI_OAUTH_DEVICE_TERMINAL);
        return OPENAI_OAUTH_DEVICE_TERMINAL;
    }
    if (auth->device_poll_in_progress)
    { (void)pthread_mutex_unlock(&auth->lock); return OPENAI_OAUTH_DEVICE_PENDING; }
    if (now < auth->device_next_poll)
    { (void)pthread_mutex_unlock(&auth->lock); return OPENAI_OAUTH_DEVICE_PENDING; }
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
    { device_poll_released(auth, generation); return OPENAI_OAUTH_DEVICE_TRANSIENT; }
    long status = 0;
    char *response = NULL;
    OpenAIOAuthTokenResult request = device_post(auth, generation,
        "/api/accounts/deviceauth/token", body, &status, &response);
    OPENSSL_cleanse(body, strlen(body));
    free(body);
    if (request == OPENAI_OAUTH_TOKEN_CANCELLED)
    { if (response) { OPENSSL_cleanse(response, strlen(response)); free(response); }
      device_poll_released(auth, generation);
      return OPENAI_OAUTH_DEVICE_CANCELLED; }
    if (request != OPENAI_OAUTH_TOKEN_OK)
    { if (response) { OPENSSL_cleanse(response, strlen(response)); free(response); }
      device_poll_released(auth, generation);
      return OPENAI_OAUTH_DEVICE_TRANSIENT; }
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
        if (response) { OPENSSL_cleanse(response, strlen(response)); free(response); }
        device_poll_released(auth, generation);
        return result;
    }
    char *authorization_code = NULL;
    char *verifier = NULL;
    if (result == OPENAI_OAUTH_DEVICE_COMPLETE &&
        parse_device_authorization(response, &authorization_code, &verifier) != 0)
        result = OPENAI_OAUTH_DEVICE_TRANSIENT;
    if (response) { OPENSSL_cleanse(response, strlen(response)); free(response); }
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
        if (tokens) { OPENSSL_cleanse(tokens, strlen(tokens)); free(tokens); }
    }
    secure_free(&authorization_code);
    secure_free(&verifier);
    if (result != OPENAI_OAUTH_DEVICE_CANCELLED)
        device_poll_finished(auth, generation, result);
    active_operation_finish(auth);
    return result;
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
    { lifecycle_finish(auth); return -1; }
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

static int needs_refresh(time_t expires_at, time_t now)
{
    if (expires_at <= now) return 1;
    return expires_at - now <= OAUTH_REFRESH_SKEW_SECONDS;
}

static int copy_access_locked(OpenAIOAuth *auth, char **access_token,
                              char **account_id)
{
    *access_token = string_dup(auth->access_token);
    if (!*access_token) return -1;
    if (auth->account_id)
    {
        *account_id = string_dup(auth->account_id);
        if (!*account_id) { secure_free(access_token); return -1; }
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
    { secure_free(&refresh); secure_free(&old_account); secure_free(&old_plan);
      return OPENAI_OAUTH_TOKEN_TRANSIENT; }
    uint64_t generation = auth->generation;
    auth->refresh_in_progress = 1;
    if (pthread_mutex_unlock(&auth->lock) != 0)
    { secure_free(&refresh); secure_free(&old_account); secure_free(&old_plan);
      return OPENAI_OAUTH_TOKEN_TRANSIENT; }
    char *json = NULL;
    OpenAIOAuthTokenResult result = exchange_token(auth, generation, "refresh_token",
                                                    refresh, NULL, NULL, &json);
    OAuthCredentials staged = {0};
    now = time(NULL);
    if (result == OPENAI_OAUTH_TOKEN_OK &&
        (now == (time_t)-1 || token_set_parse(json, refresh, old_account, old_plan,
                                              now, &staged) != 0))
        result = OPENAI_OAUTH_TOKEN_TRANSIENT;
    if (json) { OPENSSL_cleanse(json, strlen(json)); free(json); }
    secure_free(&refresh);
    secure_free(&old_account);
    secure_free(&old_plan);
    if (pthread_mutex_lock(&auth->lock) != 0)
    { credentials_clear(&staged); return OPENAI_OAUTH_TOKEN_TRANSIENT; }
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
    { (void)pthread_mutex_unlock(&auth->lock); return OPENAI_OAUTH_TOKEN_SIGNED_OUT; }
    OpenAIOAuthTokenResult result = refresh_credentials(auth, force,
                                                        access_token, account_id);
    if (pthread_mutex_unlock(&auth->lock) != 0)
    { secure_free(access_token); secure_free(account_id); return OPENAI_OAUTH_TOKEN_TRANSIENT; }
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
    { (void)pthread_mutex_unlock(&auth->lock); return -1; }
    auth->lifecycle_busy = 1;
    cancel_callback_locked(auth);
    joinable = take_callback_thread_locked(auth, &thread);
    (void)pthread_mutex_unlock(&auth->lock);
    if (join_callback_thread(thread, joinable) != 0)
    { lifecycle_finish(auth); return -1; }
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
char *openai_oauth_test_build_authorize_url(const char *state, const char *challenge)
{
    return build_authorize_url_values(state, challenge);
}

char *openai_oauth_test_pkce_challenge(const char *verifier)
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
