#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "openai_oauth.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define OPENAI_ISSUER "https://auth.openai.com"
#define OPENAI_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define OPENAI_CALLBACK_PORT 1455
#define OPENAI_CALLBACK_PATH "/auth/callback"
#define OPENAI_PROVIDER_NAME "openai"
#define OAUTH_LOGIN_TIMEOUT_SECONDS 300
#define OAUTH_REFRESH_SKEW_SECONDS 300

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} OAuthBuffer;

struct OpenAIOAuth {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t callback_thread;
    int thread_joinable;
    int callback_active;
    int callback_ready;
    int callback_rc;
    int listener_fd;
    int stop_requested;
    char *state;
    char *verifier;
    char *challenge;
    char *login_id;
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
    memset(*value, 0, strlen(*value));
    free(*value);
    *value = NULL;
}

static size_t oauth_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    OAuthBuffer *buf = userdata;
    size_t total = size * nmemb;
    if (total > SIZE_MAX - buf->len - 1) return 0;
    size_t needed = buf->len + total + 1;
    if (needed > buf->cap)
    {
        size_t cap = buf->cap ? buf->cap * 2 : 1024;
        while (cap < needed)
        {
            if (cap > SIZE_MAX / 2) return 0;
            cap *= 2;
        }
        char *grown = realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static int is_url_char(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static char *url_encode(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!value) return NULL;
    size_t len = strlen(value);
    if (len > (SIZE_MAX - 1) / 3) return NULL;
    char *encoded = malloc(len * 3 + 1);
    if (!encoded) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)value[i];
        if (is_url_char(c)) encoded[out++] = (char)c;
        else
        {
            encoded[out++] = '%';
            encoded[out++] = hex[c >> 4];
            encoded[out++] = hex[c & 0x0f];
        }
    }
    encoded[out] = '\0';
    return encoded;
}

static char *base64url_encode(const unsigned char *data, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (len > (SIZE_MAX - 2) / 4 * 3) return NULL;
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < len; i += 3)
    {
        size_t remaining = len - i;
        unsigned int value = (unsigned int)data[i] << 16;
        if (remaining > 1) value |= (unsigned int)data[i + 1] << 8;
        if (remaining > 2) value |= data[i + 2];
        out[oi++] = table[(value >> 18) & 63];
        out[oi++] = table[(value >> 12) & 63];
        if (remaining > 1) out[oi++] = table[(value >> 6) & 63];
        if (remaining > 2) out[oi++] = table[value & 63];
    }
    out[oi] = '\0';
    return out;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(const char *value, size_t len)
{
    char *decoded = malloc(len + 1);
    if (!decoded) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (value[i] == '%')
        {
            if (i + 2 >= len) { free(decoded); return NULL; }
            int high = hex_value(value[i + 1]);
            int low = hex_value(value[i + 2]);
            if (high < 0 || low < 0) { free(decoded); return NULL; }
            decoded[out++] = (char)((high << 4) | low);
            i += 2;
        }
        else if (value[i] == '+') decoded[out++] = ' ';
        else decoded[out++] = value[i];
    }
    decoded[out] = '\0';
    return decoded;
}

static char *query_value(const char *query, const char *wanted)
{
    if (!query || !wanted) return NULL;
    const char *cursor = query;
    while (*cursor)
    {
        const char *amp = strchr(cursor, '&');
        size_t part_len = amp ? (size_t)(amp - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', part_len);
        if (equals)
        {
            size_t name_len = (size_t)(equals - cursor);
            if (strlen(wanted) == name_len && strncmp(cursor, wanted, name_len) == 0)
                return url_decode(equals + 1, part_len - name_len - 1);
        }
        if (!amp) break;
        cursor = amp + 1;
    }
    return NULL;
}

static char *build_authorize_url_values(const char *state, const char *challenge)
{
    char *redirect = url_encode("http://localhost:1455/auth/callback");
    char *scope = url_encode("openid profile email offline_access");
    char *originator = url_encode("echo-ai");
    char *encoded_state = url_encode(state);
    char *encoded_challenge = url_encode(challenge);
    char *url = NULL;
    if (redirect && scope && originator && encoded_state && encoded_challenge)
    {
        if (asprintf(&url, OPENAI_ISSUER "/oauth/authorize?response_type=code&client_id=%s&"
                     "redirect_uri=%s&scope=%s&code_challenge=%s&code_challenge_method=S256&"
                     "id_token_add_organizations=true&codex_cli_simplified_flow=true&"
                     "originator=%s&state=%s", OPENAI_CLIENT_ID, redirect, scope,
                     encoded_challenge, originator, encoded_state) < 0)
            url = NULL;
    }
    free(redirect);
    free(scope);
    free(originator);
    free(encoded_state);
    free(encoded_challenge);
    return url;
}

static int random_string(char **out)
{
    unsigned char bytes[32];
    if (!out || RAND_bytes(bytes, sizeof(bytes)) != 1) return -1;
    *out = base64url_encode(bytes, sizeof(bytes));
    memset(bytes, 0, sizeof(bytes));
    return *out ? 0 : -1;
}

static int make_pkce(char **verifier, char **challenge)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (random_string(verifier) != 0) return -1;
    SHA256((const unsigned char *)*verifier, strlen(*verifier), digest);
    *challenge = base64url_encode(digest, sizeof(digest));
    memset(digest, 0, sizeof(digest));
    if (!*challenge)
    {
        secure_free(verifier);
        return -1;
    }
    return 0;
}

static char *json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item || !cJSON_IsString(item) || !item->valuestring ||
        !item->valuestring[0]) return NULL;
    return str_dup(item->valuestring);
}

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static char *base64url_decode(const char *input)
{
    size_t len = strlen(input);
    if (len == 0 || len % 4 == 1) return NULL;
    size_t out_len = len / 4 * 3;
    size_t remainder = len % 4;
    if (remainder == 2) out_len += 1;
    if (remainder == 3) out_len += 2;
    unsigned char *out = calloc(out_len + 1, 1);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < len; i += 4)
    {
        size_t remain = len - i;
        int a = base64url_value(input[i]);
        int b = remain > 1 ? base64url_value(input[i + 1]) : -1;
        int c = remain > 2 ? base64url_value(input[i + 2]) : 0;
        int d = remain > 3 ? base64url_value(input[i + 3]) : 0;
        if (a < 0 || b < 0 || (remain > 2 && c < 0) || (remain > 3 && d < 0))
        {
            free(out);
            return NULL;
        }
        unsigned int value = ((unsigned int)a << 18) | ((unsigned int)b << 12) |
                             ((unsigned int)c << 6) | (unsigned int)d;
        out[oi++] = (unsigned char)(value >> 16);
        if (remain > 2) out[oi++] = (unsigned char)(value >> 8);
        if (remain > 3) out[oi++] = (unsigned char)value;
    }
    out[out_len] = '\0';
    return (char *)out;
}

static cJSON *jwt_payload_json(const char *jwt)
{
    if (!jwt) return NULL;
    const char *dot = strchr(jwt, '.');
    if (!dot) return NULL;
    const char *payload = dot + 1;
    const char *end = strchr(payload, '.');
    if (!end || end == payload) return NULL;
    char *encoded = strndup(payload, (size_t)(end - payload));
    if (!encoded) return NULL;
    char *decoded = base64url_decode(encoded);
    free(encoded);
    if (!decoded) return NULL;
    cJSON *json = cJSON_Parse(decoded);
    free(decoded);
    return json;
}

static char *token_account_id(const char *id_token)
{
    cJSON *payload = jwt_payload_json(id_token);
    if (!payload) return NULL;
    char *account = json_string(payload, "chatgpt_account_id");
    if (account) { cJSON_Delete(payload); return account; }
    cJSON *auth = cJSON_GetObjectItemCaseSensitive(payload, "https://api.openai.com/auth");
    char *result = auth && cJSON_IsObject(auth) ? json_string(auth, "chatgpt_account_id") : NULL;
    cJSON_Delete(payload);
    return result;
}

static char *token_plan_type(const char *id_token)
{
    cJSON *payload = jwt_payload_json(id_token);
    if (!payload) return NULL;
    cJSON *auth = cJSON_GetObjectItemCaseSensitive(payload, "https://api.openai.com/auth");
    char *result = auth && cJSON_IsObject(auth) ? json_string(auth, "chatgpt_plan_type") : NULL;
    cJSON_Delete(payload);
    return result;
}

static int curl_set_common(CURL *curl, const char *url, const char *body,
                           OAuthBuffer *buffer)
{
    if (!curl || !url || !body || !buffer) return -1;
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oauth_write_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer) != CURLE_OK)
        return -1;
    return 0;
}

static int exchange_token(const char *grant_type, const char *code,
                          const char *redirect_uri, const char *verifier,
                          cJSON **response_out)
{
    char *grant = url_encode(grant_type);
    char *code_value = url_encode(code);
    char *client = url_encode(OPENAI_CLIENT_ID);
    char *body = NULL;
    if (grant && code_value && client)
    {
        if (strcmp(grant_type, "refresh_token") == 0)
            asprintf(&body, "grant_type=%s&refresh_token=%s&client_id=%s",
                     grant, code_value, client);
        else
        {
            char *redirect = url_encode(redirect_uri);
            char *encoded_verifier = url_encode(verifier);
            if (redirect && encoded_verifier)
                asprintf(&body, "grant_type=%s&code=%s&redirect_uri=%s&client_id=%s&"
                         "code_verifier=%s", grant, code_value, redirect, client,
                         encoded_verifier);
            free(redirect);
            free(encoded_verifier);
        }
    }
    free(grant);
    free(code_value);
    free(client);
    if (!body) return -1;

    CURL *curl = curl_easy_init();
    OAuthBuffer buffer = {0};
    struct curl_slist *headers = curl_slist_append(NULL,
        "Content-Type: application/x-www-form-urlencoded");
    int result = -1;
    if (curl && headers && curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_set_common(curl, OPENAI_ISSUER "/oauth/token", body, &buffer) == 0)
    {
        CURLcode perform_result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (perform_result == CURLE_OK && status >= 200 && status < 300 && buffer.data)
        {
            *response_out = cJSON_Parse(buffer.data);
            result = *response_out ? 0 : -1;
        }
    }
    if (result != 0) log_error("OpenAI OAuth token exchange failed", NULL);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buffer.data);
    free(body);
    return result;
}

static int update_from_token_json(OpenAIOAuth *auth, cJSON *json)
{
    char *access = json_string(json, "access_token");
    char *refresh = json_string(json, "refresh_token");
    char *id_token = json_string(json, "id_token");
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires_in");
    int expires_in = expires && cJSON_IsNumber(expires) ? expires->valueint : 3600;
    if (!access || (auth->refresh_token == NULL && !refresh))
    {
        secure_free(&access);
        secure_free(&refresh);
        secure_free(&id_token);
        return -1;
    }
    if (refresh) { secure_free(&auth->refresh_token); auth->refresh_token = refresh; refresh = NULL; }
    secure_free(&auth->access_token);
    auth->access_token = access;
    auth->expires_at = time(NULL) + (expires_in > 0 ? expires_in : 3600);
    if (id_token)
    {
        char *account = token_account_id(id_token);
        char *plan = token_plan_type(id_token);
        if (account) { secure_free(&auth->account_id); auth->account_id = account; }
        if (plan) { secure_free(&auth->plan_type); auth->plan_type = plan; }
        secure_free(&id_token);
    }
    return 0;
}

static char *oauth_json(const OpenAIOAuth *auth)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    int ok = cJSON_AddStringToObject(json, "access_token", auth->access_token) &&
             cJSON_AddStringToObject(json, "refresh_token", auth->refresh_token) &&
             cJSON_AddNumberToObject(json, "expires_at", (double)auth->expires_at);
    if (auth->account_id) ok = ok && cJSON_AddStringToObject(json, "account_id", auth->account_id);
    if (auth->plan_type) ok = ok && cJSON_AddStringToObject(json, "plan_type", auth->plan_type);
    char *result = ok ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    return result;
}

static int persist_locked(OpenAIOAuth *auth)
{
    if (!auth->session) return 0;
    char *data = oauth_json(auth);
    if (!data) return -1;
    int result = session_manager_save_provider_oauth(auth->session,
                                                      OPENAI_PROVIDER_NAME, data);
    memset(data, 0, strlen(data));
    free(data);
    return result;
}

static int load_json_locked(OpenAIOAuth *auth, const char *data)
{
    cJSON *json = cJSON_Parse(data);
    if (!json) return -1;
    char *access = json_string(json, "access_token");
    char *refresh = json_string(json, "refresh_token");
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires_at");
    if (!access || !refresh || !expires || !cJSON_IsNumber(expires))
    {
        secure_free(&access);
        secure_free(&refresh);
        cJSON_Delete(json);
        return -1;
    }
    secure_free(&auth->access_token);
    secure_free(&auth->refresh_token);
    auth->access_token = access;
    auth->refresh_token = refresh;
    auth->expires_at = (time_t)expires->valuedouble;
    char *account = json_string(json, "account_id");
    char *plan = json_string(json, "plan_type");
    secure_free(&auth->account_id);
    secure_free(&auth->plan_type);
    auth->account_id = account;
    auth->plan_type = plan;
    cJSON_Delete(json);
    return 0;
}

static int refresh_locked(OpenAIOAuth *auth)
{
    cJSON *json = NULL;
    if (!auth->refresh_token || exchange_token("refresh_token", auth->refresh_token,
                                               NULL, NULL, &json) != 0)
        return -1;
    int result = update_from_token_json(auth, json);
    cJSON_Delete(json);
    if (result == 0 && persist_locked(auth) != 0) result = -1;
    return result;
}

static void set_error_locked(OpenAIOAuth *auth, const char *message)
{
    secure_free(&auth->last_error);
    auth->last_error = str_dup(message ? message : "OpenAI OAuth failed");
}

static void callback_response(int fd, int status, const char *body)
{
    const char *reason = status == 200 ? "OK" : "Bad Request";
    char header[256];
    int len = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\n"
                       "Content-Type: text/html; charset=utf-8\r\n"
                       "Connection: close\r\nContent-Length: %zu\r\n\r\n",
                       status, reason, strlen(body));
    if (len > 0 && (size_t)len < sizeof(header))
    {
        (void)send(fd, header, (size_t)len, 0);
        (void)send(fd, body, strlen(body), 0);
    }
}

static int parse_callback_request(const char *request, char **code, char **state)
{
    const char *start = strstr(request, "GET ");
    if (!start) return -1;
    start += 4;
    const char *space = strchr(start, ' ');
    if (!space || strncmp(start, OPENAI_CALLBACK_PATH "?", 15) != 0) return -1;
    char *query = strndup(start + 15, (size_t)(space - start - 15));
    if (!query) return -1;
    *code = query_value(query, "code");
    *state = query_value(query, "state");
    free(query);
    if (!*code || !*state)
    {
        secure_free(code);
        secure_free(state);
        return -1;
    }
    return 0;
}

static void callback_finished(OpenAIOAuth *auth)
{
    pthread_mutex_lock(&auth->lock);
    auth->callback_active = 0;
    auth->listener_fd = -1;
    pthread_cond_broadcast(&auth->condition);
    pthread_mutex_unlock(&auth->lock);
}

static void *callback_thread_main(void *userdata)
{
    OpenAIOAuth *auth = userdata;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int result = -1;
    if (fd >= 0)
    {
        int reuse = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(OPENAI_CALLBACK_PORT);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(fd, 1) == 0)
        {
            pthread_mutex_lock(&auth->lock);
            auth->listener_fd = fd;
            auth->callback_ready = 1;
            auth->callback_rc = 0;
            int stop = auth->stop_requested;
            pthread_cond_broadcast(&auth->condition);
            pthread_mutex_unlock(&auth->lock);
            if (!stop)
            {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(fd, &read_set);
                struct timeval timeout = {.tv_sec = OAUTH_LOGIN_TIMEOUT_SECONDS, .tv_usec = 0};
                if (select(fd + 1, &read_set, NULL, NULL, &timeout) > 0)
                {
                    int client = accept(fd, NULL, NULL);
                    if (client >= 0)
                    {
                        char request[8192];
                        ssize_t received = recv(client, request, sizeof(request) - 1, 0);
                        if (received > 0)
                        {
                            request[received] = '\0';
                            char *code = NULL;
                            char *state = NULL;
                            if (parse_callback_request(request, &code, &state) == 0)
                            {
                                pthread_mutex_lock(&auth->lock);
                                int valid = auth->state && strcmp(auth->state, state) == 0;
                                char *verifier = valid ? str_dup(auth->verifier) : NULL;
                                pthread_mutex_unlock(&auth->lock);
                                if (valid && verifier)
                                {
                                    cJSON *tokens = NULL;
                                    if (exchange_token("authorization_code", code,
                                                       "http://localhost:1455/auth/callback",
                                                       verifier, &tokens) == 0)
                                    {
                                        pthread_mutex_lock(&auth->lock);
                                        if (update_from_token_json(auth, tokens) == 0 &&
                                            persist_locked(auth) == 0)
                                        {
                                            secure_free(&auth->last_error);
                                            result = 0;
                                        }
                                        else set_error_locked(auth, "Could not save OpenAI credentials");
                                        pthread_mutex_unlock(&auth->lock);
                                        cJSON_Delete(tokens);
                                    }
                                    else
                                    {
                                        pthread_mutex_lock(&auth->lock);
                                        set_error_locked(auth, "OpenAI token exchange failed");
                                        pthread_mutex_unlock(&auth->lock);
                                    }
                                }
                                else
                                {
                                    pthread_mutex_lock(&auth->lock);
                                    set_error_locked(auth, "Invalid OpenAI OAuth state");
                                    pthread_mutex_unlock(&auth->lock);
                                }
                                secure_free(&verifier);
                            }
                            else
                            {
                                pthread_mutex_lock(&auth->lock);
                                set_error_locked(auth, "Invalid OpenAI OAuth callback");
                                pthread_mutex_unlock(&auth->lock);
                            }
                            secure_free(&code);
                            secure_free(&state);
                        }
                        callback_response(client, result == 0 ? 200 : 400,
                                          result == 0 ? "OpenAI login complete. You may close this window."
                                                      : "OpenAI login failed. You may close this window.");
                        close(client);
                    }
                }
            }
        }
        else
        {
            pthread_mutex_lock(&auth->lock);
            auth->callback_ready = 1;
            auth->callback_rc = -1;
            pthread_cond_broadcast(&auth->condition);
            pthread_mutex_unlock(&auth->lock);
        }
        close(fd);
    }
    else
    {
        pthread_mutex_lock(&auth->lock);
        auth->callback_ready = 1;
        auth->callback_rc = -1;
        pthread_cond_broadcast(&auth->condition);
        pthread_mutex_unlock(&auth->lock);
    }
    callback_finished(auth);
    return NULL;
}

OpenAIOAuth *openai_oauth_create(void)
{
    OpenAIOAuth *auth = calloc(1, sizeof(OpenAIOAuth));
    if (!auth) return NULL;
    if (pthread_mutex_init(&auth->lock, NULL) != 0)
    {
        free(auth);
        return NULL;
    }
    if (pthread_cond_init(&auth->condition, NULL) != 0)
    {
        pthread_mutex_destroy(&auth->lock);
        free(auth);
        return NULL;
    }
    auth->listener_fd = -1;
    return auth;
}

void openai_oauth_destroy(OpenAIOAuth *auth)
{
    if (!auth) return;
    pthread_mutex_lock(&auth->lock);
    auth->stop_requested = 1;
    if (auth->listener_fd >= 0) shutdown(auth->listener_fd, SHUT_RDWR);
    int joinable = auth->thread_joinable;
    pthread_t thread = auth->callback_thread;
    pthread_mutex_unlock(&auth->lock);
    if (joinable) pthread_join(thread, NULL);
    secure_free(&auth->state);
    secure_free(&auth->verifier);
    secure_free(&auth->challenge);
    secure_free(&auth->login_id);
    secure_free(&auth->access_token);
    secure_free(&auth->refresh_token);
    secure_free(&auth->account_id);
    secure_free(&auth->plan_type);
    secure_free(&auth->last_error);
    pthread_cond_destroy(&auth->condition);
    pthread_mutex_destroy(&auth->lock);
    free(auth);
}

int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm)
{
    if (!auth) return -1;
    char *data = sm ? session_manager_load_provider_oauth(sm, OPENAI_PROVIDER_NAME) : NULL;
    pthread_mutex_lock(&auth->lock);
    auth->session = sm;
    int result = data ? load_json_locked(auth, data) : 0;
    pthread_mutex_unlock(&auth->lock);
    if (data) { memset(data, 0, strlen(data)); free(data); }
    return result;
}

int openai_oauth_start(OpenAIOAuth *auth, char **authorization_url,
                       char **login_id)
{
    if (!auth || !authorization_url || !login_id) return -1;
    *authorization_url = NULL;
    *login_id = NULL;
    pthread_mutex_lock(&auth->lock);
    if (auth->callback_active)
    {
        pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    if (auth->thread_joinable)
    {
        pthread_t previous = auth->callback_thread;
        auth->thread_joinable = 0;
        pthread_mutex_unlock(&auth->lock);
        pthread_join(previous, NULL);
        pthread_mutex_lock(&auth->lock);
    }
    char *state = NULL;
    char *verifier = NULL;
    char *challenge = NULL;
    char *id = NULL;
    if (random_string(&state) != 0 || make_pkce(&verifier, &challenge) != 0 ||
        random_string(&id) != 0)
    {
        secure_free(&state); secure_free(&verifier); secure_free(&challenge); secure_free(&id);
        pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    char *url = build_authorize_url_values(state, challenge);
    if (!url)
    {
        secure_free(&state); secure_free(&verifier); secure_free(&challenge); secure_free(&id);
        pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    auth->state = state;
    auth->verifier = verifier;
    auth->challenge = challenge;
    auth->login_id = id;
    secure_free(&auth->last_error);
    auth->callback_active = 1;
    auth->callback_ready = 0;
    auth->callback_rc = -1;
    auth->stop_requested = 0;
    if (pthread_create(&auth->callback_thread, NULL, callback_thread_main, auth) != 0)
    {
        auth->callback_active = 0;
        secure_free(&auth->state); secure_free(&auth->verifier);
        secure_free(&auth->challenge); secure_free(&auth->login_id);
        free(url);
        pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    auth->thread_joinable = 1;
    while (!auth->callback_ready) pthread_cond_wait(&auth->condition, &auth->lock);
    if (auth->callback_rc != 0)
    {
        auth->stop_requested = 1;
        pthread_mutex_unlock(&auth->lock);
        pthread_join(auth->callback_thread, NULL);
        pthread_mutex_lock(&auth->lock);
        auth->thread_joinable = 0;
        free(url);
        url = NULL;
    }
    else
    {
        *authorization_url = url;
        *login_id = str_dup(auth->login_id);
        if (!*login_id) { free(url); *authorization_url = NULL; }
    }
    pthread_mutex_unlock(&auth->lock);
    return *authorization_url ? 0 : -1;
}

OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error)
{
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    if (!auth) return OPENAI_OAUTH_SIGNED_OUT;
    pthread_mutex_lock(&auth->lock);
    if (account_id && auth->account_id) *account_id = str_dup(auth->account_id);
    if (plan_type && auth->plan_type) *plan_type = str_dup(auth->plan_type);
    if (error && auth->last_error) *error = str_dup(auth->last_error);
    OpenAIOAuthState state = auth->callback_active ? OPENAI_OAUTH_PENDING :
        (auth->refresh_token ? OPENAI_OAUTH_SIGNED_IN : OPENAI_OAUTH_SIGNED_OUT);
    pthread_mutex_unlock(&auth->lock);
    return state;
}

int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id)
{
    if (!auth || !access_token || !account_id) return -1;
    *access_token = NULL;
    *account_id = NULL;
    pthread_mutex_lock(&auth->lock);
    if (!auth->access_token || !auth->refresh_token)
    {
        pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    if (auth->expires_at <= time(NULL) + OAUTH_REFRESH_SKEW_SECONDS && refresh_locked(auth) != 0)
    {
        pthread_mutex_unlock(&auth->lock);
        return -1;
    }
    *access_token = str_dup(auth->access_token);
    if (auth->account_id) *account_id = str_dup(auth->account_id);
    pthread_mutex_unlock(&auth->lock);
    if (!*access_token)
    {
        secure_free(account_id);
        return -1;
    }
    return 0;
}

int openai_oauth_logout(OpenAIOAuth *auth)
{
    if (!auth) return -1;
    pthread_mutex_lock(&auth->lock);
    int result = auth->session ? session_manager_delete_provider_oauth(auth->session,
                                                                        OPENAI_PROVIDER_NAME) : 0;
    secure_free(&auth->access_token);
    secure_free(&auth->refresh_token);
    secure_free(&auth->account_id);
    secure_free(&auth->plan_type);
    secure_free(&auth->last_error);
    auth->expires_at = 0;
    pthread_mutex_unlock(&auth->lock);
    return result;
}

#ifdef OPENAI_OAUTH_TEST
char *openai_oauth_test_build_authorize_url(const char *state, const char *challenge)
{
    return build_authorize_url_values(state, challenge);
}

int openai_oauth_test_parse_callback(const char *request, char **code, char **state)
{
    return parse_callback_request(request, code, state);
}
#endif
