/*
 * python_execute.c - Python execution tool: runs model-provided code in a
 * subprocess with a hard timeout and returns its stdout/stderr.
 * Depends on: tool.h, safety, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    int timeout;
} PythonCtx;

static ToolResult *python_execute_execute(Tool *self, const char *args_json)
{
    PythonCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *code_json = cJSON_GetObjectItem(args, "code");
    if (!code_json || !cJSON_IsString(code_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'code' argument", "validation_error");
    }

    char *code = str_dup(cJSON_GetStringValue(code_json));
    cJSON_Delete(args);
    if (!code) return tool_result_error("oom", "execution_error");

    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
        return tool_result_error("pipe creation failed", "execution_error");

    pid_t pid = fork();
    if (pid < 0)
    {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return tool_result_error("fork failed", "execution_error");
    }

    if (pid == 0)
    {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        for (int i = 3; i < 256; i++) close(i);

        execlp("python3", "python3", "-c", code, NULL);
        execlp("python", "python", "-c", code, NULL);
        _exit(127);
    }

    /* code is only needed by the child (execlp); free it in the parent,
     * which otherwise leaks it on every exit path (caught by Linux LSan). */
    free(code);

    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    int timeout = ctx->timeout > 0 ? ctx->timeout : 30;
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
            return tool_result_error("Python execution timed out", "timeout");
        }
        usleep(100000);
        elapsed++;
    }

    char stdout_buf[65536];
    char stderr_buf[65536];
    size_t out_len = 0;
    size_t err_len = 0;

    ssize_t n;
    while ((n = read(stdout_pipe[0], stdout_buf + out_len,
                     sizeof(stdout_buf) - out_len - 1)) > 0)
        out_len += (size_t)n;
    while ((n = read(stderr_pipe[0], stderr_buf + err_len,
                     sizeof(stderr_buf) - err_len - 1)) > 0)
        err_len += (size_t)n;

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    stdout_buf[out_len] = '\0';
    stderr_buf[err_len] = '\0';

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (exit_code != 0)
    {
        char *err = NULL;
        if (asprintf(&err, "Python exit code %d\n%s", exit_code,
                     err_len > 0 ? stderr_buf : stdout_buf) < 0)
            err = str_dup("Python execution error");
        ToolResult *tr = tool_result_error(err, "execution_error");
        free(err);
        return tr;
    }

    if (err_len > 0)
    {
        size_t total = out_len + err_len + 32;
        char *combined = malloc(total);
        if (combined)
        {
            if (out_len > 0 && err_len > 0)
                snprintf(combined, total, "%s\n(stderr: %s)", stdout_buf, stderr_buf); // NOLINT(cert-err33-c)
            else if (out_len > 0)
                snprintf(combined, total, "%s", stdout_buf); // NOLINT(cert-err33-c)
            else
                snprintf(combined, total, "(stderr: %s)", stderr_buf); // NOLINT(cert-err33-c)
            ToolResult *tr = tool_result_create(combined);
            free(combined);
            return tr;
        }
    }

    ToolResult *tr = tool_result_create(out_len > 0 ? stdout_buf : "(no output)");
    return tr;
}

static void python_execute_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_python_execute_create - construct the python_execute tool
 * @safety: accepted for interface uniformity only; ignored by this tool
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy().
 */
Tool *tool_python_execute_create(SafetyConfig *safety)
{
    (void)safety;
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    PythonCtx *ctx = calloc(1, sizeof(PythonCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->timeout = 30;

    t->name = str_dup("python_execute");
    t->description = str_dup("Execute Python code and return the output");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"code\":{\"type\":\"string\",\"description\":\"Python code to execute\"}"
        "},\"required\":[\"code\"]}"
    );
    t->execute = python_execute_execute;
    t->destroy = python_execute_destroy;
    t->ctx = ctx;
    return t;
}
