/*
 * fake_cdp.c - fake CDP peer implementation (see fake_cdp.h).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "fake_cdp.h"

#define POLL_INTERVAL_MS 100
#define READ_CHUNK 65536

static void fake_write_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t w = write(fd, data + off, len - off);
        if (w > 0)
        {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return;
    }
}

static FakeCdpRule *fake_find_rule(FakeCdp *f, cJSON *req,
                                   const char *method)
{
    /* Rules match the method name, or the JS expression carried in
     * Runtime.evaluate params (the browser layer's evaluate-based
     * polling/extraction all use that method). */
    cJSON *params = cJSON_GetObjectItem(req, "params");
    cJSON *expr = cJSON_IsObject(params)
                      ? cJSON_GetObjectItem(params, "expression") : NULL;
    const char *expression = cJSON_IsString(expr) ? expr->valuestring : NULL;

    for (int i = 0; i < f->rule_count; i++)
    {
        const char *m = f->rules[i].match;
        if (!m) return &f->rules[i];
        if (method && strstr(method, m)) return &f->rules[i];
        if (expression && strstr(expression, m)) return &f->rules[i];
    }
    return NULL;
}

static void fake_respond(FakeCdp *f, int id, const char *method,
                         cJSON *req)
{
    FakeCdpRule *rule = fake_find_rule(f, req, method);
    if (!rule || rule->drop) return;

    cJSON *msg = cJSON_CreateObject();
    if (!msg) return;
    cJSON_AddNumberToObject(msg, "id", id);
    if (rule->error_message)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddNumberToObject(err, "code", -32000);
        cJSON_AddStringToObject(err, "message", rule->error_message);
        cJSON_AddItemToObject(msg, "error", err);
    }
    else if (rule->payload)
    {
        cJSON_AddItemReferenceToObject(msg, "result", rule->payload);
    }
    else
    {
        cJSON *empty = cJSON_CreateObject();
        cJSON_AddItemToObject(msg, "result", empty);
    }
    char *line = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!line) return;

    size_t len = strlen(line);
    char *framed = realloc(line, len + 2);
    if (!framed)
    {
        free(line);
        return;
    }
    /* Pipe protocol: every message ends with a NUL frame byte. */
    framed[len] = '\0';
    framed[len + 1] = '\0';
    line = framed;
    len++;

    if (rule->prelude)
    {
        /* Prelude and response are separate protocol frames: the NUL
         * between them is part of the framing, not decoration. */
        size_t plen = strlen(rule->prelude);
        char *both = malloc(plen + 1 + len + 1);
        if (both)
        {
            memcpy(both, rule->prelude, plen);
            both[plen] = '\0';
            memcpy(both + plen + 1, line, len);
            both[plen + 1 + len] = '\0';
            free(line);
            line = both;
            len += plen + 1;
        }
    }

    int pieces = rule->chunks > 0 ? rule->chunks : 1;
    if (pieces > (int)len) pieces = (int)len;
    size_t per = len / (size_t)pieces;
    size_t extra = len % (size_t)pieces;
    size_t off = 0;
    for (int i = 0; i < pieces; i++)
    {
        size_t n = per + (i == 0 ? extra : 0);
        fake_write_all(f->fd, line + off, n);
        off += n;
        if (i + 1 < pieces) usleep(5000);
    }
    free(line);
}

static void *fake_cdp_main(void *arg)
{
    FakeCdp *f = arg;
    char *buf = NULL;
    size_t buflen = 0;
    size_t bufcap = 0;

    for (;;)
    {
        struct pollfd pfd = {.fd = f->fd, .events = POLLIN};
        int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        char chunk[READ_CHUNK];
        ssize_t n = read(f->fd, chunk, sizeof(chunk));
        if (n <= 0) break;

        if (buflen + (size_t)n + 1 > bufcap)
        {
            size_t nc = bufcap ? bufcap * 2 : 8192;
            while (nc < buflen + (size_t)n + 1) nc *= 2;
            char *nb = realloc(buf, nc);
            if (!nb) break;
            buf = nb;
            bufcap = nc;
        }
        memcpy(buf + buflen, chunk, (size_t)n);
        buflen += (size_t)n;
        buf[buflen] = '\0';

        for (;;)
        {
            char *nul = memchr(buf, '\0', buflen);
            if (!nul) break;
            size_t linelen = (size_t)(nul - buf);

            cJSON *req = cJSON_Parse(buf);
            if (req)
            {
                cJSON *id = cJSON_GetObjectItem(req, "id");
                cJSON *method = cJSON_GetObjectItem(req, "method");
                if (cJSON_IsNumber(id) && cJSON_IsString(method))
                {
                    f->requests_seen++;
                    fake_respond(f, (int)id->valuedouble,
                                 method->valuestring, req);
                }
                cJSON_Delete(req);
            }
            memmove(buf, nul + 1, buflen - linelen);
            buflen -= linelen + 1;
        }
    }
    free(buf);
    return NULL;
}

FakeCdp *fake_cdp_start(int *client_fd_out, FakeCdpRule *rules, int count)
{
    if (!client_fd_out) return NULL;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return NULL;

    FakeCdp *f = calloc(1, sizeof(FakeCdp));
    if (!f)
    {
        close(sv[0]);
        close(sv[1]);
        return NULL;
    }
    f->fd = sv[0];
    f->rules = rules;
    f->rule_count = count;

    if (pthread_create(&f->thread, NULL, fake_cdp_main, f) != 0)
    {
        free(f);
        close(sv[0]);
        close(sv[1]);
        return NULL;
    }
    *client_fd_out = sv[1];
    return f;
}

void fake_cdp_stop(FakeCdp *f)
{
    if (!f) return;
    close(f->fd);
    pthread_join(f->thread, NULL);
    free(f);
}
