/*
 * http_client.h - libcurl building blocks shared by every provider/tool
 * that performs HTTP requests: a bounded growable response buffer with a
 * curl write callback. Replaces nine hand-rolled copies of the same
 * pattern (ollama, openai, openai_compatible, openai_oauth, web_fetch,
 * rest_api, search_brave/duckduckgo/tavily) that had drifted apart in
 * overflow and limit safety. Depends on: libcurl.
 */

#ifndef ECHO_HTTP_CLIENT_H
#define ECHO_HTTP_CLIENT_H

#include <stddef.h>
#include <curl/curl.h>

typedef struct {
    char *data;    /* NUL-terminated accumulated body; caller-owned */
    size_t len;    /* bytes accumulated, excluding the NUL */
    size_t cap;    /* allocated capacity */
    size_t limit;  /* 0 = unbounded; an append past the limit is refused */
    int too_large; /* 1 after an append was refused by the limit */
} HttpBuffer;

/**
 * http_buffer_append - accumulate bytes into a growable response buffer
 * @b: buffer to append to; must be non-NULL. May be zero-initialized,
 *   or initialized with a designated `.limit` to bound the response.
 * @bytes: bytes to append; NULL is allowed only when @length is 0.
 *
 * Appends @length bytes and keeps the buffer NUL-terminated. Refuses
 * (with the limit breach flagged in @b->too_large) any append that would
 * push the buffer past @b->limit, and refuses any size arithmetic that
 * would overflow. Capacity grows geometrically, capped at limit + 1 when
 * a limit is set. Safe for stack-declared buffers freed with
 * http_buffer_free(); no shared state; thread-safe on distinct buffers.
 *
 * Return: 0 on success, -1 on NULL argument, OOM, or size violation
 * (limit breach or arithmetic overflow). On -1 the buffer is unchanged
 * except that @b->too_large may be set.
 */
int http_buffer_append(HttpBuffer *b, const void *bytes, size_t length);

/**
 * http_buffer_free - release a buffer's storage
 * @b: buffer to free, or NULL (no-op). @b is reset to zero so it can be
 *   freed again safely.
 *
 * Return: void; never fails.
 */
void http_buffer_free(HttpBuffer *b);

/**
 * http_buffer_write_cb - curl CURLOPT_WRITEFUNCTION for HttpBuffer
 * @ptr: chunk delivered by libcurl.
 * @size: element size.
 * @nmemb: element count.
 * @userdata: must point to an HttpBuffer.
 *
 * Appends the delivered chunk via http_buffer_append(). Returns the
 * chunk length on success so the transfer proceeds; returns 0 on OOM or
 * size violation, which aborts the transfer with a write-error. A
 * too-large body is therefore reported as a failed transfer, not a
 * silently truncated one.
 *
 * Return: total bytes accepted, or 0 to abort the transfer.
 */
size_t http_buffer_write_cb(void *ptr, size_t size, size_t nmemb,
                            void *userdata);

#endif
