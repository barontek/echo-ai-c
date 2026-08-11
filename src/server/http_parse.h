/*
 * http_parse.h - HTTP request parsing contracts.
 * Depends on: server.h.
 */

#ifndef ECHO_HTTP_PARSE_H
#define ECHO_HTTP_PARSE_H

#include <stddef.h>

#include "server.h"

/**
 * parse_http - parse buffered request bytes into the client
 * @client: connection whose buf holds the request.
 *
 * Parses the request line and headers once (size-bounded), then the
 * body once content-length bytes have arrived.
 *
 * Return: 1 when the request is complete, 0 when more bytes are needed,
 * -1 on malformed or oversized input.
 */
int parse_http(Client *client);

/**
 * client_append - append bytes to the client's request buffer
 * @client: connection.
 * @data: bytes to append.
 * @len: byte count.
 *
 * Grows the buffer geometrically, refusing input beyond the combined
 * header+body cap.
 *
 * Return: 0 on success, -1 on overflow or allocation failure.
 */
int client_append(Client *client, const char *data, size_t len);

#ifdef SERVER_TEST
/**
 * server_test_parse_chunks - test seam driving the streaming parser
 * @chunks: array of byte fragments.
 * @lengths: per-chunk lengths, or NULL to strlen.
 * @count: number of chunks.
 * @method: receives the parsed method.
 * @path: receives the parsed path.
 * @body: receives the parsed body.
 *
 * Return: 1 when the request parsed completely, 0 when more data is
 * needed, -1 on parse failure.
 */
int server_test_parse_chunks(const char **chunks, const size_t *lengths, int count,
                             char *method, size_t method_size,
                             char *path, size_t path_size,
                             char *body, size_t body_size);
#endif

#endif /* ECHO_HTTP_PARSE_H */
