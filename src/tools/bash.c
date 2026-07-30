#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} BashCtx;

static int run_with_timeout(const char *command, int timeout_secs, char **output)
{
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0)
    {
        if (setpgid(0, 0) != 0) _exit(126);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    pipefd[1] = -1;
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH)
    {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        return -1;
    }
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0)
    {
        (void)kill(-pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        return -1;
    }

    int max_wait = timeout_secs > 0 ? timeout_secs : 30;
    int status = 0;
    int child_done = 0;
    int pipe_open = 1;
    int descendants_signaled = 0;
    char result_buf[65536];
    size_t total = 0;
    struct timespec start = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    {
        (void)kill(-pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        return -1;
    }

    while (!child_done || pipe_open)
    {
        char chunk[4096];
        while (pipe_open)
        {
            ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
            if (n > 0)
            {
                size_t available = sizeof(result_buf) - 1 - total;
                size_t copy_len = (size_t)n < available ? (size_t)n : available;
                if (copy_len > 0)
                {
                    memcpy(result_buf + total, chunk, copy_len);
                    total += copy_len;
                }
                continue;
            }
            if (n == 0)
            {
                close(pipefd[0]);
                pipe_open = 0;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            {
                close(pipefd[0]);
                pipe_open = 0;
            }
            break;
        }

        if (!child_done)
        {
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid)
                child_done = 1;
            else if (ret < 0 && errno != EINTR)
            {
                if (pipe_open) close(pipefd[0]);
                return -1;
            }
        }

        if (child_done && !descendants_signaled)
        {
            (void)kill(-pid, SIGTERM);
            descendants_signaled = 1;
        }

        struct timespec now = {0};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        {
            (void)kill(-pid, SIGKILL);
            if (!child_done) (void)waitpid(pid, &status, 0);
            if (pipe_open) close(pipefd[0]);
            return -1;
        }
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= (double)max_wait)
        {
            (void)kill(-pid, SIGKILL);
            if (!child_done) (void)waitpid(pid, &status, 0);
            if (pipe_open) close(pipefd[0]);
            return -2;
        }

        if (!child_done || pipe_open)
        {
            struct pollfd pfd = {.fd = pipe_open ? pipefd[0] : -1,
                                 .events = POLLIN | POLLHUP};
            int poll_rc = poll(&pfd, 1, 50);
            if (poll_rc < 0 && errno != EINTR)
            {
                (void)kill(-pid, SIGKILL);
                if (!child_done) (void)waitpid(pid, &status, 0);
                if (pipe_open) close(pipefd[0]);
                return -1;
            }
        }
    }

    if (descendants_signaled)
    {
        struct timespec grace = {.tv_sec = 0, .tv_nsec = 100000000L};
        while (nanosleep(&grace, &grace) != 0 && errno == EINTR) {}
        if (kill(-pid, 0) == 0) (void)kill(-pid, SIGKILL);
    }

    result_buf[total] = '\0';
    *output = str_dup(result_buf);
    if (!*output) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static ToolResult *bash_execute(Tool *self, const char *args_json)
{
    BashCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *cmd_json = cJSON_GetObjectItem(args, "command");
    if (!cmd_json || !cJSON_IsString(cmd_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'command' argument", "validation_error");
    }

    char *command = str_dup(cJSON_GetStringValue(cmd_json));

    if (!safety_check_command(ctx->safety, command))
    {
        cJSON_Delete(args);
        free(command);
        return tool_result_error("command rejected by safety policy", "policy_denied");
    }

    cJSON_Delete(args);

    char *output = NULL;
    int rc = run_with_timeout(command, ctx->safety->max_execution_time, &output);
    free(command);

    if (rc == -2)
    {
        free(output);
        return tool_result_error("command timed out", "timeout");
    }

    char *result = NULL;
    if (rc == 0)
    {
        if (asprintf(&result, "Exit code: %d\n%s", rc, output ? output : "") < 0)
            result = NULL;
    }
    else
    {
        if (asprintf(&result, "Exit code: %d\n%s", rc, output ? output : "(no output)") < 0)
            result = NULL;
    }

    free(output);

    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;
}

static void bash_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

Tool *tool_bash_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    BashCtx *ctx = calloc(1, sizeof(BashCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("bash");
    t->description = str_dup("Execute a shell command");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"}"
        "},\"required\":[\"command\"]}"
    );
    t->execute = bash_execute;
    t->destroy = bash_destroy;
    t->ctx = ctx;
    return t;
}
