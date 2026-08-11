/*
 * serve_static.h - static file serving contracts.
 * Depends on: server.h.
 */

#ifndef ECHO_SERVE_STATIC_H
#define ECHO_SERVE_STATIC_H

#include "server.h"

/**
 * serve_static - serve a static file from the frontend dist directory
 * @client: connection to respond on.
 * @path: request path (must be absolute and traversal-free).
 * @ctx: server context (unused currently, kept for signature symmetry).
 *
 * Resolves the file beneath frontend/dist, rejecting path traversal and
 * symlink escapes, and sends it with a MIME-typed 200 or an error
 * response. Files over 10 MB are refused.
 *
 * Return: void; responds directly on @client.
 */
void serve_static(Client *client, const char *path, ServerContext *ctx);

#ifdef SERVER_TEST
/**
 * server_test_static_path_safe - test seam for path-traversal defense
 * @path: candidate path.
 *
 * Return: 1 when the path is safe to serve, 0 otherwise.
 */
int server_test_static_path_safe(const char *path);

/**
 * server_test_open_static_file_beneath - test seam for symlink-safe open
 * @root: document root directory.
 * @path: absolute file path beneath root.
 *
 * Return: an fd for the resolved file, or -1 on refusal/failure.
 */
int server_test_open_static_file_beneath(const char *root, const char *path);
#endif

#endif /* ECHO_SERVE_STATIC_H */
