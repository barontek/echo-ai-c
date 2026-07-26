#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} BashCtx;

static int run_with_timeout(const char *command, int timeout_secs, char **output)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    int max_wait = timeout_secs > 0 ? timeout_secs : 30;
    int slept = 0;
    int status = 0;

    while (slept < max_wait)
    {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) break;
        sleep(1);
        slept++;
    }

    if (slept >= max_wait)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pipefd[0]);
        return -2;
    }

    char buf[65536];
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], buf + total, sizeof(buf) - total - 1)) > 0)
        total += n;
    buf[total] = '\0';
    close(pipefd[0]);

    *output = str_dup(buf);
    return WEXITSTATUS(status);
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
