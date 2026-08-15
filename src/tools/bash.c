/*
 * bash.c - shell command execution tool with safety checks and a hard
 * timeout on the child process group. Depends on: tool.h, safety,
 * string_utils, logging.
 */

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
#include <limits.h>
#include <stdint.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} BashCtx;

/* T4a: output tail budget. Output beyond this is spilled to a temp
 * file and the model is pointed at it, instead of being silently
 * truncated by a fixed buffer (the pre-T4a 64 KB behavior). */
#define BASH_TAIL_BYTES 8192

/*
 * bash_output_cleanup - release a spill temp file (if any)
 * @tmp_path: malloc'd spill path, or NULL
 * @tmpf: open spill FILE, or NULL
 *
 * Unlinks the temp file and frees the path. Used on every error exit
 * from run_with_timeout so a failed command leaves nothing behind.
 */
static void bash_output_cleanup(char *tmp_path, FILE *tmpf)
{
    if (tmpf) (void)fclose(tmpf);
    if (tmp_path)
    {
        unlink(tmp_path);
        free(tmp_path);
    }
}

static int run_with_timeout(const char *command, int timeout_secs,
                            char **output, char **full_out_path)
{
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) < 0)
    {
        /* C12: pre-launch failures used to collapse into "Exit code: -1"
         * with no explanation; the errno context is logged here and
         * signalled to the caller as -3. */
        log_error("bash: pipe creation failed", "err", strerror(errno), NULL);
        return -3;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("bash: fork failed", "err", strerror(errno), NULL);
        close(pipefd[0]);
        close(pipefd[1]);
        return -3;
    }

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
        log_error("bash: setpgid failed", "err", strerror(errno), NULL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        return -3;
    }
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0)
    {
        log_error("bash: fcntl failed", "err", strerror(errno), NULL);
        (void)kill(-pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        return -3;
    }

    int max_wait = timeout_secs > 0 ? timeout_secs : 30;
    int status = 0;
    int child_done = 0;
    int pipe_open = 1;
    int descendants_signaled = 0;
    char tail[BASH_TAIL_BYTES + 1];
    size_t tail_len = 0;
    uint64_t total = 0;
    char *tmp_path = NULL;
    FILE *tmpf = NULL;
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
                /* read() cannot exceed the buffer, but clamping makes
                 * the bound visible to the compiler so the window
                 * copies below are provably in-bounds. */
                size_t chunk_len = (size_t)n;
                if (chunk_len > sizeof(chunk)) chunk_len = sizeof(chunk);
                total += chunk_len;

                /* T4a: once the output outgrows the tail budget, start a
                 * temp file holding the FULL output and keep only the
                 * rolling tail in memory. */
                if (!tmp_path &&
                    total > (uint64_t)BASH_TAIL_BYTES)
                {
                    char tmpl[] = "/tmp/echo-bash-XXXXXX";
                    int fd = mkstemp(tmpl);
                    if (fd < 0)
                    {
                        (void)kill(-pid, SIGKILL);
                        if (!child_done) (void)waitpid(pid, &status, 0);
                        if (pipe_open) close(pipefd[0]);
                        return -4;
                    }
                    tmp_path = str_dup(tmpl);
                    if (!tmp_path)
                    {
                        close(fd);
                        (void)kill(-pid, SIGKILL);
                        if (!child_done) (void)waitpid(pid, &status, 0);
                        if (pipe_open) close(pipefd[0]);
                        return -4;
                    }
                    tmpf = fdopen(fd, "wb");
                    if (!tmpf)
                    {
                        close(fd);
                        unlink(tmp_path);
                        free(tmp_path);
                        tmp_path = NULL;
                        (void)kill(-pid, SIGKILL);
                        if (!child_done) (void)waitpid(pid, &status, 0);
                        if (pipe_open) close(pipefd[0]);
                        return -4;
                    }
                    /* Everything seen so far (the full window) goes
                     * into the spill file before streaming continues. */
                    if (tail_len > 0 &&
                        fwrite(tail, 1, tail_len, tmpf) != tail_len)
                    {
                        bash_output_cleanup(tmp_path, tmpf);
                        (void)kill(-pid, SIGKILL);
                        if (!child_done) (void)waitpid(pid, &status, 0);
                        if (pipe_open) close(pipefd[0]);
                        return -4;
                    }
                }
                if (tmpf &&
                    fwrite(chunk, 1, chunk_len, tmpf) != chunk_len)
                {
                    bash_output_cleanup(tmp_path, tmpf);
                    (void)kill(-pid, SIGKILL);
                    if (!child_done) (void)waitpid(pid, &status, 0);
                    if (pipe_open) close(pipefd[0]);
                    return -4;
                }

                /* Rolling tail window: keep the newest BASH_TAIL_BYTES
                 * bytes. Overflow keeps the newest chunk_len bytes of
                 * the old window plus the whole chunk; chunk_len is
                 * always < BASH_TAIL_BYTES (4096 < 8192), so the
                 * memmove/memcpy sizes are bounded. */
                if (tail_len + chunk_len <= BASH_TAIL_BYTES)
                {
                    memcpy(tail + tail_len, chunk, chunk_len);
                    tail_len += chunk_len;
                }
                else
                {
                    size_t keep_old = BASH_TAIL_BYTES - chunk_len;
                    memmove(tail, tail + tail_len - keep_old, keep_old);
                    memcpy(tail + keep_old, chunk, chunk_len);
                    tail_len = BASH_TAIL_BYTES;
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
                bash_output_cleanup(tmp_path, tmpf);
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
            bash_output_cleanup(tmp_path, tmpf);
            return -1;
        }
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= (double)max_wait)
        {
            (void)kill(-pid, SIGKILL);
            if (!child_done) (void)waitpid(pid, &status, 0);
            if (pipe_open) close(pipefd[0]);
            bash_output_cleanup(tmp_path, tmpf);
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
                bash_output_cleanup(tmp_path, tmpf);
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

    if (tmpf && fclose(tmpf) != 0)
    {
        unlink(tmp_path);
        free(tmp_path);
        return -4;
    }
    if (tmp_path)
    {
        /* Trim the tail window to start at a line boundary; the dropped
         * partial line is already captured in the spill file. */
        size_t first_nl = 0;
        while (first_nl < tail_len && tail[first_nl] != '\n')
            first_nl++;
        if (first_nl < tail_len)
        {
            size_t keep = tail_len - (first_nl + 1);
            memmove(tail, tail + first_nl + 1, keep);
            tail_len = keep;
        }
    }

    tail[tail_len] = '\0';
    *output = str_dup(tail);
    if (!*output)
    {
        bash_output_cleanup(tmp_path, NULL);
        return -1;
    }
    *full_out_path = tmp_path;
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

    /* T4: optional per-call timeout override, capped at the policy
     * maximum so the LLM cannot escalate beyond the safety limit. */
    int timeout_secs = ctx->safety->max_execution_time;
    cJSON *timeout_json = cJSON_GetObjectItem(args, "timeout");
    if (timeout_json)
    {
        double dv = timeout_json->valuedouble;
        if (!cJSON_IsNumber(timeout_json) || dv < 1.0 ||
            dv > (double)INT_MAX || dv != (double)(int)dv)
        {
            cJSON_Delete(args);
            free(command);
            return tool_result_error(
                "timeout must be a whole number of seconds",
                "validation_error");
        }
        int requested = (int)dv;
        if (ctx->safety->max_execution_time <= 0 ||
            requested < ctx->safety->max_execution_time)
            timeout_secs = requested;
    }

    cJSON_Delete(args);

    char *output = NULL;
    char *full_out = NULL;
    int rc = run_with_timeout(command, timeout_secs, &output, &full_out);
    free(command);

    if (rc == -2)
    {
        free(output);
        return tool_result_error("command timed out", "timeout");
    }

    if (rc == -3)
    {
        /* C12: launch failures get a real error, not a fake exit code;
         * the syscall context is already in the server log. */
        free(output);
        return tool_result_error("failed to launch command",
                                 "execution_error");
    }

    if (rc == -4)
    {
        /* T4a: output could not be captured (spill file failure). */
        free(output);
        return tool_result_error("failed to capture command output",
                                 "execution_error");
    }

    char *result = NULL;
    if (full_out)
    {
        /* T4a: the model is pointed at the spill file for the full
         * output; the file is intentionally kept (pi does the same).
         * Cleanup on server exit is a follow-up (see PI_PARITY_PLAN). */
        if (asprintf(&result, "Exit code: %d\n%s\n\n[Full output: %s]",
                     rc, output ? output : "", full_out) < 0)
            result = NULL;
        free(full_out);
    }
    else if (rc == 0)
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

/**
 * tool_bash_create - construct the bash tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_bash_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    BashCtx *ctx = calloc(1, sizeof(BashCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("bash");
    t->description = str_dup("Execute a shell command; optional timeout in seconds, capped at the safety limit");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
        "\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds, capped at the safety limit\"}"
        "},\"required\":[\"command\"]}"
    );
    t->execute = bash_execute;
    t->destroy = bash_destroy;
    t->ctx = ctx;
    return t;
}
