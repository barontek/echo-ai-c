/*
 * git.c - git operations tool: runs status/diff/log/add/commit/push/pull/
 * branch/stash in a subprocess with a timeout and returns the output.
 * Depends on: tool.h, safety, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    int timeout;
} GitCtx;

static char *run_git(const char *const *argv, int timeout)
{
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return NULL;
    }

    if (pid == 0)
    {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        for (int i = 3; i < 256; i++) close(i);
        execvp("git", (char *const *)argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    int status = 0;
    pid_t result;
    int elapsed = 0;

    while ((result = waitpid(pid, &status, WNOHANG)) == 0) // NOLINT(clang-analyzer-deadcode.DeadStores)
    {
        if (elapsed >= timeout * 10)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            return NULL;
        }
        usleep(100000);
        elapsed++;
    }

    char buf[65536];
    size_t total = 0;
    ssize_t n;
    while ((n = read(stdout_pipe[0], buf + total, sizeof(buf) - total - 1)) > 0)
        total += (size_t)n;
    close(stdout_pipe[0]);

    char err_buf[4096];
    size_t err_total = 0;
    while ((n = read(stderr_pipe[0], err_buf + err_total, sizeof(err_buf) - err_total - 1)) > 0)
        err_total += (size_t)n;
    close(stderr_pipe[0]);

    buf[total] = '\0';
    err_buf[err_total] = '\0';

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (exit_code != 0 && exit_code != 127)
    {
        if (err_total > 0)
        {
            char *combined = NULL;
            if (asprintf(&combined, "git exit code %d\n%s", exit_code, err_buf) >= 0)
                return combined;
        }
        return str_dup(buf);
    }

    if (exit_code == 127) return str_dup("Error: git not found");

    return str_dup(buf);
}

static char *git_status(void)
{
    const char *argv[] = {"git", "status", "--short", NULL};
    return run_git(argv, 30);
}

static char *git_diff(const char *path)
{
    if (path && path[0])
    {
        const char *argv[] = {"git", "diff", "--no-color", path, NULL};
        return run_git(argv, 30);
    }
    const char *argv[] = {"git", "diff", "--no-color", NULL};
    return run_git(argv, 30);
}

static char *git_log(int count)
{
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "-%d", count > 0 ? count : 10); // NOLINT(cert-err33-c)
    const char *argv[] = {"git", "log", "--oneline", count_str, NULL};
    return run_git(argv, 30);
}

static char *git_add(const char *path)
{
    if (!path || !path[0]) path = ".";
    const char *argv[] = {"git", "add", path, NULL};
    return run_git(argv, 30);
}

static char *git_commit(const char *message)
{
    if (!message || !message[0]) message = "update";
    const char *argv[] = {"git", "commit", "-m", message, NULL};
    return run_git(argv, 30);
}

static char *git_push(void)
{
    const char *argv[] = {"git", "push", NULL};
    return run_git(argv, 60);
}

static char *git_pull(void)
{
    const char *argv[] = {"git", "pull", "--ff-only", NULL};
    return run_git(argv, 60);
}

static char *git_branch(void)
{
    const char *argv[] = {"git", "branch", "-a", NULL};
    return run_git(argv, 30);
}

static char *git_stash(void)
{
    const char *argv[] = {"git", "stash", "list", NULL};
    return run_git(argv, 30);
}

static ToolResult *git_execute(Tool *self, const char *args_json)
{
    (void)self;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *op_json = cJSON_GetObjectItem(args, "operation");
    if (!op_json || !cJSON_IsString(op_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'operation' argument", "validation_error");
    }

    char *op = str_dup(cJSON_GetStringValue(op_json));

    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    cJSON *msg_json = cJSON_GetObjectItem(args, "message");
    cJSON *count_json = cJSON_GetObjectItem(args, "count");

    char *path_str = NULL;
    if (path_json && cJSON_IsString(path_json))
        path_str = str_dup(cJSON_GetStringValue(path_json));

    char *msg_str = NULL;
    if (msg_json && cJSON_IsString(msg_json))
        msg_str = str_dup(cJSON_GetStringValue(msg_json));

    int count_val = count_json && cJSON_IsNumber(count_json) ? count_json->valueint : 10;
    cJSON_Delete(args);

    char *result = NULL;

    if (strcmp(op, "status") == 0)
        result = git_status();
    else if (strcmp(op, "diff") == 0)
        result = git_diff(path_str);
    else if (strcmp(op, "log") == 0)
        result = git_log(count_val);
    else if (strcmp(op, "add") == 0)
        result = git_add(path_str);
    else if (strcmp(op, "commit") == 0)
        result = git_commit(msg_str);
    else if (strcmp(op, "push") == 0)
        result = git_push();
    else if (strcmp(op, "pull") == 0)
        result = git_pull();
    else if (strcmp(op, "branch") == 0)
        result = git_branch();
    else if (strcmp(op, "stash") == 0)
        result = git_stash();
    else
    {
        free(op);
        free(path_str);
        free(msg_str);
        return tool_result_error("unknown git operation", "validation_error");
    }

    free(op);
    free(path_str);
    free(msg_str);

    if (!result)
        return tool_result_error("git operation failed or timed out", "execution_error");

    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;
}

static void git_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_git_create - construct the git tool
 * @safety: accepted for interface uniformity only; ignored by this tool
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy().
 */
Tool *tool_git_create(SafetyConfig *safety)
{
    (void)safety;
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    GitCtx *ctx = calloc(1, sizeof(GitCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->timeout = 60;

    t->name = str_dup("git");
    t->description = str_dup("Git operations: status, diff, log, add, commit, push, pull, branch, stash");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"operation\":{\"type\":\"string\",\"enum\":[\"status\",\"diff\",\"log\",\"add\",\"commit\",\"push\",\"pull\",\"branch\",\"stash\"],\"description\":\"Git operation\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"File path (for add, diff)\"},"
        "\"message\":{\"type\":\"string\",\"description\":\"Commit message\"},"
        "\"count\":{\"type\":\"integer\",\"description\":\"Number of log entries (default 10)\"}"
        "},\"required\":[\"operation\"]}"
    );
    t->execute = git_execute;
    t->destroy = git_destroy;
    t->ctx = ctx;
    return t;
}
