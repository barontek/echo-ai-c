/*
 * http_client.c - shared libcurl response-buffer plumbing.
 * Depends on: libcurl.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "http_client.h"

int http_buffer_append(HttpBuffer *b, const void *bytes, size_t length)
{
    if (!b || (!bytes && length != 0)) return -1;
    if (b->limit && (length > b->limit || b->len > b->limit - length))
    {
        b->too_large = 1;
        return -1;
    }
    /* len + length + 1 must not wrap: reject len+length == SIZE_MAX. */
    if (length > SIZE_MAX - b->len - 1U) return -1;
    size_t needed = b->len + length + 1U;
    if (needed > b->cap)
    {
        size_t capacity = b->cap ? b->cap : 1024U;
        while (capacity < needed)
        {
            if (b->limit && capacity > (b->limit + 1U) / 2U)
            {
                capacity = b->limit + 1U;
                break;
            }
            if (capacity > SIZE_MAX / 2U)
            {
                capacity = needed;
                break;
            }
            capacity *= 2U;
        }
        if (capacity < needed) return -1;
        char *grown = realloc(b->data, capacity);
        if (!grown) return -1;
        b->data = grown;
        b->cap = capacity;
    }
    if (length != 0) memcpy(b->data + b->len, bytes, length);
    b->len += length;
    b->data[b->len] = '\0';
    return 0;
}

void http_buffer_free(HttpBuffer *b)
{
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

size_t http_buffer_write_cb(void *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
    if (size != 0U && nmemb > SIZE_MAX / size) return 0U;
    size_t total = size * nmemb;
    return http_buffer_append(userdata, ptr, total) == 0 ? total : 0U;
}
