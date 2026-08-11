/*
 * serve_static.c - static file serving for the frontend: path
 * traversal defense, symlink-safe descent beneath the document root,
 * and MIME typing.
 * Depends on: server_internal.h (Client), core response helpers.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "server_internal.h"
#include "server.h"
#include "serve_static.h"
#include "../utils/logging.h"

static const char *mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0)   return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0)  return "image/png";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0)  return "image/x-icon";
    return "application/octet-stream";
}

static int static_request_path_safe(const char *path)
{
    if (!path || path[0] != '/' || strchr(path, '\\')) return 0;

    const char *segment = path;
    while (*segment)
    {
        while (*segment == '/') segment++;
        const char *end = strchr(segment, '/');
        size_t len = end ? (size_t)(end - segment) : strlen(segment);
        if ((len == 1 && segment[0] == '.') ||
            (len == 2 && segment[0] == '.' && segment[1] == '.'))
            return 0;
        if (!end) break;
        segment = end;
    }
    return 1;
}

static int open_static_file_beneath(const char *root, const char *filepath)
{
    size_t root_len = strlen(root);
    if (strncmp(filepath, root, root_len) != 0 ||
        (filepath[root_len] != '\0' && filepath[root_len] != '/'))
        return -1;

    const char *relative = filepath + root_len;
    while (*relative == '/') relative++;
    if (!*relative || strlen(relative) >= PATH_MAX) return -1;

    char path_copy[PATH_MAX];
    memcpy(path_copy, relative, strlen(relative) + 1);
    int dir_fd = open(root, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) return -1;

    char *save = NULL;
    char *segment = strtok_r(path_copy, "/", &save);
    while (segment)
    {
        char *next = strtok_r(NULL, "/", &save);
        int flags = O_RDONLY | O_NOFOLLOW;
        if (next) flags |= O_DIRECTORY;
        int next_fd = openat(dir_fd, segment, flags);
        close(dir_fd);
        if (next_fd < 0) return -1;
        dir_fd = next_fd;
        segment = next;
    }
    return dir_fd;
}

void serve_static(Client *client, const char *path, ServerContext *ctx)
{
    (void)ctx;
    if (!static_request_path_safe(path))
    {
        server_response_error(client, 400, "invalid static path");
        return;
    }

    char root[PATH_MAX];
    if (!realpath("frontend/dist", root))
    {
        server_response_error(client, 404, "not found");
        return;
    }

    char candidate[PATH_MAX];
    int written = 0;
    if (strcmp(path, "/") == 0)
        written = snprintf(candidate, sizeof(candidate), "%s/index.html", root);
    else
        written = snprintf(candidate, sizeof(candidate), "%s%s", root, path);
    if (written < 0 || (size_t)written >= sizeof(candidate))
    {
        server_response_error(client, 400, "static path too long");
        return;
    }

    char filepath[PATH_MAX];
    if (!realpath(candidate, filepath))
    {
        written = snprintf(candidate, sizeof(candidate), "%s%s/index.html", root, path);
        if (written < 0 || (size_t)written >= sizeof(candidate) ||
            !realpath(candidate, filepath))
        {
            server_response_error(client, 404, "not found");
            return;
        }
    }

    size_t root_len = strlen(root);
    if (strncmp(filepath, root, root_len) != 0 ||
        (filepath[root_len] != '\0' && filepath[root_len] != '/'))
    {
        server_response_error(client, 404, "not found");
        return;
    }

    int fd = open_static_file_beneath(root, filepath);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        if (fd >= 0) close(fd);
        server_response_error(client, 404, "not found");
        return;
    }

    FILE *f = fdopen(fd, "rb");
    if (!f)
    {
        close(fd);
        server_response_error(client, 404, "not found");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || fsize > 10485760) {
        fclose(f);
        server_response_error(client, 500, "file too large");
        return;
    }

    char *content = malloc((size_t)fsize + 1);
    if (!content) {
        fclose(f);
        server_response_error(client, 500, "oom");
        return;
    }

    size_t read = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    content[read] = '\0';

    const char *ct = mime_type(filepath);
    server_response(client, 200, ct, content);
    free(content);
}

#ifdef SERVER_TEST
int server_test_static_path_safe(const char *path)
{
    return static_request_path_safe(path);
}

int server_test_open_static_file_beneath(const char *root, const char *path)
{
    return open_static_file_beneath(root, path);
}
#endif
