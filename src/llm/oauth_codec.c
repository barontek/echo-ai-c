/*
 * oauth_codec.c - URL encoding, PKCE, and base64url primitives for the
 * OpenAI OAuth flows. Pure helpers: no state, no I/O.
 * Depends on: OpenSSL, cJSON (types only).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "oauth_codec.h"
#include "oauth_vault.h"
#include "openai_oauth_internal.h"

char *string_dup(const char *value)
{
    if (!value) return NULL;
    size_t len = strlen(value);
    if (len == SIZE_MAX) return NULL;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static int is_url_char(unsigned char value)
{
    return isalnum(value) || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

char *url_encode(const char *value)
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

char *pkce_challenge(const char *verifier)
{
    if (!verifier) return NULL;
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    if (!SHA256((const unsigned char *)verifier, strlen(verifier), digest)) return NULL;
    char *challenge = base64url_encode(digest, sizeof(digest));
    OPENSSL_cleanse(digest, sizeof(digest));
    return challenge;
}

int random_string(char **output)
{
    unsigned char bytes[32] = {0};
    if (!output) return -1;
    *output = NULL;
    if (RAND_bytes(bytes, (int)sizeof(bytes)) != 1) return -1;
    *output = base64url_encode(bytes, sizeof(bytes));
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return *output ? 0 : -1;
}

int make_pkce(char **verifier, char **challenge)
{
    if (!verifier || !challenge) return -1;
    *verifier = NULL;
    *challenge = NULL;
    if (random_string(verifier) != 0) return -1;
    *challenge = pkce_challenge(*verifier);
    if (!*challenge) {
        secure_free(verifier);
        return -1;
    }
    return 0;
}

char *build_authorize_url_values(const char *state, const char *challenge)
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

char *url_decode_exact(const unsigned char *value, size_t len)
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
            if (index + 2 >= len) {
                free(decoded);
                return NULL;
            }
            int high = hex_value(value[index + 1]);
            int low = hex_value(value[index + 2]);
            if (high < 0 || low < 0) {
                free(decoded);
                return NULL;
            }
            byte = (unsigned char)((high << 4) | low);
            index += 2;
        }
        else if (byte == '+') byte = ' ';
        if (byte == 0 || byte < 0x20 || byte == 0x7f)
         {
            free(decoded);
            return NULL;
        }
        decoded[output++] = (char)byte;
    }
    decoded[output] = '\0';
    return decoded;
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

unsigned char *base64url_decode(const char *input, size_t *output_len)
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
         {
            free(output);
            return NULL;
        }
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
